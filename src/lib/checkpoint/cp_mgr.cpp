/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <urcu.h>

#include <homestore/homestore.hpp>
#include <homestore/meta_service.hpp>
#include <homestore/checkpoint/cp_mgr.hpp>
#include <homestore/homestore.hpp>
#include "common/homestore_assert.hpp"
#include "common/homestore_config.hpp"
#include "common/resource_mgr.hpp"
#include "cp_internal.hpp"

namespace homestore {
thread_local std::stack< CP* > CPGuard::t_cp_stack;

CPManager::CPManager() :
        m_metrics{std::make_unique< CPMgrMetrics >()},
        m_wd_cp{std::make_unique< CPWatchdog >(this)},
        m_sb{"CPSuperBlock"} {
    meta_service().register_handler(
        "CPSuperBlock",
        [this](meta_blk* mblk, sisl::byte_view buf, size_t size) { on_meta_blk_found(std::move(buf), (void*)mblk); },
        nullptr);

    resource_mgr().register_dirty_buf_exceed_cb(
        [this]([[maybe_unused]] int64_t dirty_buf_count) { this->trigger_cp_flush(false /* false */); });

    start_cp_thread();
}

CPManager::~CPManager() { HS_REL_ASSERT(!m_cur_cp, "CPManager is tiering down without calling shutdown"); }

void CPManager::start(bool first_time_boot) {
    if (first_time_boot) {
        m_sb.create(sizeof(cp_mgr_super_block));
        create_first_cp();
        m_sb.write();
    }

    LOGINFO("cp timer is set to {} usec", HS_DYNAMIC_CONFIG(generic.cp_timer_us));
    m_cp_timer_hdl = iomanager.schedule_global_timer(
        HS_DYNAMIC_CONFIG(generic.cp_timer_us) * 1000, true, nullptr /*cookie*/, iomgr::reactor_regex::all_worker,
        [this](void*) { trigger_cp_flush(false /* false */); }, true /* wait_to_schedule */);
}

void CPManager::on_meta_blk_found(const sisl::byte_view& buf, void* meta_cookie) {
    m_sb.load(buf, meta_cookie);
    create_first_cp();
    HS_REL_ASSERT_EQ(m_sb->magic, cp_sb_magic, "Invalid Checkpoint metablk, magic mismatch");
    HS_REL_ASSERT_EQ(m_sb->version, cp_sb_version, "Invalid version of checkpoint metablk");
}

void CPManager::create_first_cp() {
    m_cur_cp = new CP(this);
    m_cur_cp->m_cp_status = cp_status_t::cp_io_ready;
    m_cur_cp->m_cp_id = m_sb->m_last_flushed_cp + 1;
}

void CPManager::shutdown() {
    LOGINFO("Stopping cp timer");
    iomanager.cancel_timer(m_cp_timer_hdl, true);
    m_cp_timer_hdl = iomgr::null_timer_handle;
    m_cp_shutdown_initiated = true;

    LOGINFO("Trigger cp flush");
    auto success = trigger_cp_flush(true /* force */).get();
    HS_REL_ASSERT_EQ(success, true, "CP Flush failed");
    LOGINFO("Trigger cp done");

    delete (m_cur_cp);
    rcu_xchg_pointer(&m_cur_cp, nullptr);

    m_metrics.reset();
    if (m_wd_cp) {
        m_wd_cp->stop();
        m_wd_cp.reset();
    }
}

void CPManager::register_consumer(cp_consumer_t consumer_id, std::unique_ptr< CPCallbacks > callbacks) {
    size_t idx = (size_t)consumer_id;
    m_cp_cb_table[idx] = std::move(callbacks);
    if (m_cp_cb_table[idx]) {
        m_cur_cp->m_contexts[idx] = std::move(m_cp_cb_table[idx]->on_switchover_cp(nullptr, m_cur_cp));
    }
}

[[nodiscard]] CPGuard CPManager::cp_guard() { return CPGuard{this}; }

CP* CPManager::cp_io_enter() {
    rcu_read_lock();
    auto cp = get_cur_cp();

    HS_DBG_ASSERT_NE((void*)cp, nullptr, "get_cur_cp returned null, cp_io_enter() after shutdown?");
    if (!cp) {
        rcu_read_unlock();
        return nullptr;
    }
    cp_ref(cp);
    rcu_read_unlock();

    return cp;
}

void CPManager::cp_ref(CP* cp) {
    cp->m_enter_cnt.increment(1);
#ifndef NDEBUG
    auto status = cp->m_cp_status.load();
    HS_DBG_ASSERT((status == cp_status_t::cp_io_ready || status == cp_status_t::cp_trigger ||
                   status == cp_status_t::cp_flush_prepare),
                  "cp status {}", status);
#endif
}

void CPManager::cp_io_exit(CP* cp) {
    HS_DBG_ASSERT_NE(cp->m_cp_status, cp_status_t::cp_flushing);
    if (cp->m_enter_cnt.decrement_testz(1) && (cp->m_cp_status == cp_status_t::cp_flush_prepare)) {
        cp_start_flush(cp);
    }
}

CP* CPManager::get_cur_cp() {
    CP* p = rcu_dereference(m_cur_cp);
    return p;
}

folly::Future< bool > CPManager::trigger_cp_flush(bool force) {
    // check the state of previous CP flush
    bool expected = false;
    auto ret = m_in_flush_phase.compare_exchange_strong(expected, true);
    if (!ret) {
        // There is already an existing CP on-going, but if force is set, we create a back-to-back CP.
        if (force) {
            std::unique_lock< std::mutex > lk(trigger_cp_mtx);
            auto cur_cp = cp_guard();
            HS_DBG_ASSERT_NE(cur_cp->m_cp_status, cp_status_t::cp_flush_prepare);
            // If multiple threads call trigger, they all get the future from the same promise.
            if (!cur_cp->m_cp_waiting_to_trigger) {
                cur_cp->m_comp_promise = std::move(folly::SharedPromise< bool >{});
                cur_cp->m_cp_waiting_to_trigger = true;
            }
            return cur_cp->m_comp_promise.getFuture();
        } else {
            return folly::makeFuture< bool >(false);
        }
    }

    folly::Future< bool > ret_fut = folly::Future< bool >::makeEmpty();
    {
        auto cur_cp = cp_guard();
        cur_cp->m_cp_status = cp_status_t::cp_trigger;
        HS_PERIODIC_LOG(INFO, cp, "<<<<<<<<<<< Triggering flush of the CP {}", cur_cp->to_string());
        COUNTER_INCREMENT(*m_metrics, cp_cnt, 1);
        m_cp_start_time = Clock::now();

        /* allocate a new cp */
        auto new_cp = new CP(this);
        {
            std::unique_lock< std::mutex > lk(trigger_cp_mtx);
            new_cp->m_cp_id = cur_cp->m_cp_id + 1;

            HS_PERIODIC_LOG(DEBUG, cp, "Create New CP session", new_cp->id());
            size_t idx{0};
            for (auto& consumer : m_cp_cb_table) {
                if (consumer) { new_cp->m_contexts[idx] = std::move(consumer->on_switchover_cp(cur_cp.get(), new_cp)); }
                ++idx;
            }

            HS_PERIODIC_LOG(DEBUG, cp, "CP Attached completed, proceed to exit cp critical section");
            if (cur_cp->m_cp_waiting_to_trigger) {
                // Triggered because of back-2-back CP, generate a different future. Actual future which it was attached
                // originally by the caller will be untouched and completed upto CP completion/
                ret_fut = folly::makeFuture< bool >(true);
            } else {
                cur_cp->m_comp_promise = std::move(folly::SharedPromise< bool >{});
                ret_fut = cur_cp->m_comp_promise.getFuture();
            }
            cur_cp->m_cp_status = cp_status_t::cp_flush_prepare;
            new_cp->m_cp_status = cp_status_t::cp_io_ready;
            rcu_xchg_pointer(&m_cur_cp, new_cp);
            synchronize_rcu();
        }
        // At this point we are sure that there is no thread working on prev_cp without incrementing the cp_enter cnt
    }

    HS_PERIODIC_LOG(DEBUG, cp, "CP critical section done, doing cp_io_exit");
    return ret_fut;
}

void CPManager::cp_start_flush(CP* cp) {
    std::vector< folly::Future< bool > > futs;
    HS_PERIODIC_LOG(INFO, cp, "Starting CP {} flush", cp->id());
    cp->m_cp_status = cp_status_t::cp_flushing;

    for (auto& consumer : m_cp_cb_table) {
        if (consumer) { futs.emplace_back(std::move(consumer->cp_flush(cp))); }
    }

    folly::collectAllUnsafe(futs).thenValue([this, cp](auto) {
        // All consumers have flushed for the cp
        on_cp_flush_done(cp);
    });
}

void CPManager::on_cp_flush_done(CP* cp) {
    HS_DBG_ASSERT_EQ(cp->m_cp_status, cp_status_t::cp_flushing);
    cp->m_cp_status = cp_status_t::cp_flush_done;

    iomanager.run_on_forget(pick_blocking_io_fiber(), [this, cp]() {
        // Persist the superblock with this flushed cp information
        ++(m_sb->m_last_flushed_cp);
        m_sb.write();

        cleanup_cp(cp);

        // Setting promise will cause the CP manager destructor to cleanup
        // before getting a chance to do the checking if shutdown has been
        // initiated or not.
        auto shutdown_initiated = m_cp_shutdown_initiated.load();
        auto promise = std::move(cp->m_comp_promise);

        m_wd_cp->reset_cp();
        delete cp;

        promise.setValue(true);
        if (shutdown_initiated) {
            // If shutdown initiated, dont trigger another CP.
            // Dont access any cp state after this.
            return;
        }
        m_in_flush_phase = false;

        // Trigger CP in case there is one back to back CP
        {
            auto cur_cp = cp_guard();
            if (cur_cp.get() == nullptr) { return; }
            m_wd_cp->set_cp(cur_cp.get());
            if (cur_cp->m_cp_waiting_to_trigger) {
                HS_PERIODIC_LOG(INFO, cp, "Triggering back to back CP");
                COUNTER_INCREMENT(*m_metrics, back_to_back_cps, 1);
                trigger_cp_flush(false);
            }
        }
    });
}

void CPManager::cleanup_cp(CP* cp) {
    cp->m_cp_status = cp_status_t::cp_cleaning;
    for (auto& consumer : m_cp_cb_table) {
        if (consumer) { consumer->cp_cleanup(cp); }
    }
}

void CPManager::start_cp_thread() {
    // Start WBCache flush threads
    struct Context {
        std::condition_variable cv;
        std::mutex mtx;
        int32_t thread_cnt{0};
    };
    auto ctx = std::make_shared< Context >();

    // Start a reactor with 9 fibers (8 for sync io)
    iomanager.create_reactor("cp_io", iomgr::INTERRUPT_LOOP, 8u, [this, ctx](bool is_started) {
        if (is_started) {
            {
                std::unique_lock< std::mutex > lk{ctx->mtx};
                auto v = iomanager.sync_io_capable_fibers();
                m_cp_io_fibers.insert(m_cp_io_fibers.end(), v.begin(), v.end());
                ++(ctx->thread_cnt);
            }
            ctx->cv.notify_one();
        }
    });

    {
        std::unique_lock< std::mutex > lk{ctx->mtx};
        ctx->cv.wait(lk, [&ctx] { return (ctx->thread_cnt == 1); });
    }
}

iomgr::io_fiber_t CPManager::pick_blocking_io_fiber() const {
    static thread_local std::random_device s_rd{};
    static thread_local std::default_random_engine s_re{s_rd()};
    static auto rand_fiber = std::uniform_int_distribution< size_t >(0, m_cp_io_fibers.size() - 1);
    return m_cp_io_fibers[rand_fiber(s_re)];
}

//////////////////////////////////////// CP Guard class ////////////////////////////////////////////
CPGuard::CPGuard(CPManager* mgr) {
    if (t_cp_stack.empty()) {
        // First CP in this thread stack.
        m_cp = mgr->cp_io_enter();
    } else {
        // Nested CP sections
        m_cp = t_cp_stack.top();
        m_cp->m_cp_mgr->cp_ref(m_cp);
    }
    t_cp_stack.push(m_cp);
    m_pushed = true; // m_pushed represented if this is added to current thread stack
}

CPGuard::~CPGuard() {
    if (m_pushed && !t_cp_stack.empty()) {
        //        HS_DBG_ASSERT_EQ((void*)m_cp, (void*)t_cp_stack.top(), "CPGuard mismatch of CP pointers");
        t_cp_stack.pop();
    }
    if (m_cp) { m_cp->m_cp_mgr->cp_io_exit(m_cp); }
}

CPGuard::CPGuard(const CPGuard& other) {
    m_cp = other.m_cp;
    m_pushed = false;
    m_cp->m_cp_mgr->cp_ref(m_cp);
}

CPGuard CPGuard::operator=(const CPGuard& other) {
    m_cp = other.m_cp;
    m_pushed = false;
    m_cp->m_cp_mgr->cp_ref(m_cp);
    return *this;
}

CP& CPGuard::operator*() { return *get(); }
CP* CPGuard::operator->() { return get(); }
CPContext* CPGuard::context(cp_consumer_t consumer) { return get()->context(consumer); }

CP* CPGuard::get() {
    HS_DBG_ASSERT_NE((void*)m_cp, (void*)nullptr, "CPGuard get on empty CP pointer");
    if (!m_pushed) {
        // m_pushed is false in case cp guard is moved from one thread to other
        t_cp_stack.push(m_cp);
        m_pushed = true;
    }
    return m_cp;
}

//////////////////////////////////////// CP Watchdog class //////////////////////////////////////////
CPWatchdog::CPWatchdog(CPManager* cp_mgr) :
        m_cp{nullptr}, m_cp_mgr{cp_mgr}, m_timer_sec{HS_DYNAMIC_CONFIG(generic.cp_watchdog_timer_sec)} {
    LOGINFO("CP watchdog timer setting to : {} seconds", m_timer_sec);
    m_timer_hdl =
        iomanager.schedule_global_timer(m_timer_sec * 1000 * 1000 * 1000, true, nullptr, iomgr::reactor_regex::all_user,
                                        [this](void* cookie) { cp_watchdog_timer(); });
}

void CPWatchdog::reset_cp() {
    std::unique_lock< std::shared_mutex > lk{m_cp_mtx};
    m_cp = nullptr;
    m_progress_pct = 0;
}

void CPWatchdog::set_cp(CP* cp) {
    std::unique_lock< std::shared_mutex > lk{m_cp_mtx};
    m_cp = cp;
    m_last_state_ch_time = Clock::now();
}

void CPWatchdog::stop() {
    iomanager.cancel_timer(m_timer_hdl);
    m_timer_hdl = iomgr::null_timer_handle;
    {
        std::unique_lock< std::shared_mutex > lk{m_cp_mtx};
        m_cp = nullptr;
    }
}

void CPWatchdog::cp_watchdog_timer() {
    std::unique_lock< std::shared_mutex > lk{m_cp_mtx};

    // check if any cp to track
    if (m_cp == nullptr) { return; }
    const auto status = m_cp->get_status();
    if ((status != cp_status_t::cp_flush_prepare) || (status != cp_status_t::cp_flushing)) { return; }

    uint32_t cum_pct{0};
    uint32_t count{0};
    for (auto& consumer : m_cp_mgr->consumer_list()) {
        if (consumer) {
            ++count;
            cum_pct += consumer->cp_progress_percent();
        }
    }
    if (m_progress_pct > cum_pct / count) {
        // We are making progress in flushing the data.
        m_progress_pct = cum_pct / count;
        return;
    }

    if (get_elapsed_time_ms(m_last_state_ch_time) >= m_timer_sec * 1000) {
        LOGINFO("cp progress percent {} is not changed. time elapsed {}, cp state={} ", m_progress_pct,
                get_elapsed_time_ms(m_last_state_ch_time), m_cp->to_string());
    }

    // check if enough time passed since last state change
    uint32_t max_time_multiplier = 12;
    if (get_elapsed_time_ms(m_last_state_ch_time) < max_time_multiplier * m_timer_sec * 1000) {
        uint32_t repair_attempted{0};
        for (auto& consumer : m_cp_mgr->consumer_list()) {
            if (consumer) {
                const auto pct = consumer->cp_progress_percent();
                if (pct != 100) {
                    consumer->repair_slow_cp();
                    ++repair_attempted;
                }
            }
            if (repair_attempted) { return; }
        }

        HS_REL_ASSERT(0, "cp seems to be stuck. CP State={} total time elapsed {}", m_cp->to_string(),
                      get_elapsed_time_ms(m_last_state_ch_time));
    }
}

cp_id_t CPContext::id() const { return m_cp->id(); }

} // namespace homestore
