﻿// Learning routines:
//
// 1) Automatic generation of game records in .bin format
// → "gensfen" command
//
// 2) Learning evaluation function parameters from the generated .bin files
// → "learn" command
//
// → Shuffle in the teacher phase is also an extension of this command.
// Example) "learn shuffle"
//
// 3) Automatic generation of fixed traces
// → "makebook think" command
// → implemented in extra/book/book.cpp
//
// 4) Post-station automatic review mode
// → I will not be involved in the engine because it is a problem that the GUI should assist.
// etc..

#include "learn.h"

#include "autograd.h"
#include "sfen_reader.h"

#include "misc.h"
#include "position.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "search.h"
#include "timeman.h"

#include "nnue/evaluate_nnue.h"
#include "nnue/evaluate_nnue_learner.h"

#include "syzygy/tbprobe.h"

#include <chrono>
#include <climits>
#include <cmath>    // std::exp(),std::pow(),std::log()
#include <cstring>  // memcpy()
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <iostream>
#include <mutex>

#if defined (_OPENMP)
#include <omp.h>
#endif

using namespace std;

template <typename T>
T operator +=(std::atomic<T>& x, const T rhs)
{
    T old = x.load(std::memory_order_consume);

    // It is allowed that the value is rewritten from other thread at this timing.
    // The idea that the value is not destroyed is good.
    T desired = old + rhs;
    while (!x.compare_exchange_weak(old, desired, std::memory_order_release, std::memory_order_consume))
        desired = old + rhs;
    return desired;
}
template <typename T>
T operator -= (std::atomic<T>& x, const T rhs) { return x += -rhs; }

namespace Learner
{
    static double winning_probability_coefficient = 1.0 / PawnValueEg / 4.0 * std::log(10.0);

    // Score scale factors. ex) If we set src_score_min_value = 0.0,
    // src_score_max_value = 1.0, dest_score_min_value = 0.0,
    // dest_score_max_value = 10000.0, [0.0, 1.0] will be scaled to [0, 10000].
    static double src_score_min_value = 0.0;
    static double src_score_max_value = 1.0;
    static double dest_score_min_value = 0.0;
    static double dest_score_max_value = 1.0;

    // A constant used in elmo (WCSC27). Adjustment required.
    // Since elmo does not internally divide the expression, the value is different.
    // You can set this value with the learn command.
    // 0.33 is equivalent to the constant (0.5) used in elmo (WCSC27)
    static double elmo_lambda_low = 1.0;
    static double elmo_lambda_high = 1.0;
    static double elmo_lambda_limit = 32000;

    static double max_grad = 1.0;

    // Using stockfish's WDL with win rate model instead of sigmoid
    static bool use_wdl = false;

    struct Loss
    {
        double value() const
        {
            return m_loss.value;
        }

        double grad() const
        {
            return m_loss.grad;
        }

        uint64_t count() const
        {
            return m_count;
        }

        Loss& operator += (const ValueWithGrad<double>& rhs)
        {
            std::unique_lock lock(m_mutex);

            m_loss += rhs.abs();
            m_count += 1;

            return *this;
        }

        Loss& operator += (const Loss& rhs)
        {
            std::unique_lock lock(m_mutex);

            m_loss += rhs.m_loss.abs();
            m_count += rhs.m_count;

            return *this;
        }

        void reset()
        {
            std::unique_lock lock(m_mutex);

            m_loss = ValueWithGrad<double>{ 0.0, 0.0 };
            m_count = 0;
        }

        template <typename StreamT>
        void print(const std::string& prefix, StreamT& s) const
        {
            s << "  - " << prefix << "_loss       = " << m_loss.value / (double)m_count << endl;
            s << "  - " << prefix << "_grad_norm  = " << m_loss.grad / (double)m_count << endl;
        }

    private:
        ValueWithGrad<double> m_loss{ 0.0, 0.0 };
        uint64_t m_count{0};
        std::mutex m_mutex;

    };

    static void append_files_from_dir(
        std::vector<std::string>& filenames,
        const std::string& base_dir,
        const std::string& target_dir)
    {
        string kif_base_dir = Path::combine(base_dir, target_dir);

        namespace sys = std::filesystem;
        sys::path p(kif_base_dir); // Origin of enumeration
        std::for_each(sys::directory_iterator(p), sys::directory_iterator(),
            [&](const sys::path& path) {
                if (sys::is_regular_file(path))
                    filenames.push_back(Path::combine(target_dir, path.filename().generic_string()));
            });
    }

    static void rebase_files(
        std::vector<std::string>& filenames,
        const std::string& base_dir)
    {
        for (auto& file : filenames)
        {
            file = Path::combine(base_dir, file);
        }
    }

    static double calculate_lambda(double teacher_signal)
    {
        // If the evaluation value in deep search exceeds elmo_lambda_limit
        // then apply elmo_lambda_high instead of elmo_lambda_low.
        const double lambda =
            (std::abs(teacher_signal) >= elmo_lambda_limit)
            ? elmo_lambda_high
            : elmo_lambda_low;

        return lambda;
    }

    static ValueWithGrad<double> get_loss(Value shallow, Value teacher_signal, int result, int ply)
    {
        using namespace Learner::Autograd::UnivariateStatic;

        /*
        auto q_ = sigmoid(VariableParameter<double, 0>{} * winning_probability_coefficient);
        auto p_ = sigmoid(ConstantParameter<double, 1>{} * winning_probability_coefficient);
        auto t_ = (ConstantParameter<double, 2>{} + 1.0) * 0.5;
        auto lambda_ = ConstantParameter<double, 3>{};
        auto loss_ = pow(lambda_ * (q_ - p_) + (1.0 - lambda_) * (q_ - t_), 2.0);
        */

        /*
        auto q_ = VariableParameter<double, 0>{};
        auto p_ = ConstantParameter<double, 1>{};
        auto loss_ = pow(q_ - p_, 2.0) * (1.0 / (2400.0 * 2.0 * 600.0));
        */

        const double epsilon = 1e-12;

        auto q_ = sigmoid(VariableParameter<double, 0>{} * winning_probability_coefficient);
        auto p_ = sigmoid(ConstantParameter<double, 1>{} * winning_probability_coefficient);
        auto t_ = (ConstantParameter<double, 2>{} + 1.0) * 0.5;
        auto lambda_ = ConstantParameter<double, 3>{};
        auto teacher_entropy_ = -(p_ * log(p_ + epsilon) + (1.0 - p_) * log(1.0 - p_ + epsilon));
        auto outcome_entropy_ = -(t_ * log(t_ + epsilon) + (1.0 - t_) * log(1.0 - t_ + epsilon));
        auto teacher_loss_ = -(p_ * log(q_) + (1.0 - p_) * log(1.0 - q_));
        auto outcome_loss_ = -(t_ * log(q_) + (1.0 - t_) * log(1.0 - q_));
        auto result_ = lambda_ * teacher_loss_ + (1.0 - lambda_) * outcome_loss_;
        auto entropy_ = lambda_ * teacher_entropy_ + (1.0 - lambda_) * outcome_entropy_;
        auto loss_ = result_ - entropy_;

        auto args = std::tuple((double)shallow, (double)teacher_signal, (double)result, calculate_lambda(teacher_signal));
        return loss_.eval(args);
    }

    static auto get_loss(
        Value teacher_signal,
        Value shallow,
        const PackedSfenValue& psv)
    {
        return get_loss(shallow, teacher_signal, psv.game_result, psv.gamePly);
    }

    // Class to generate sfen with multiple threads
    struct LearnerThink
    {
        struct Params
        {
            // Mini batch size size. Be sure to set it on the side that uses this class.
            uint64_t mini_batch_size = LEARN_MINI_BATCH_SIZE;

            // Option to exclude early stage from learning
            int reduction_gameply = 1;

            // If the absolute value of the evaluation value of the deep search
            // of the teacher phase exceeds this value, discard the teacher phase.
            int eval_limit = 32000;

            // Flag whether to dig a folder each time the evaluation function is saved.
            // If true, do not dig the folder.
            bool save_only_once = false;

            bool shuffle = true;

            bool verbose = false;

            double newbob_decay = 0.5;
            int newbob_num_trials = 4;
            uint64_t auto_lr_drop = 0;

            std::string best_nn_directory;

            uint64_t eval_save_interval = LEARN_EVAL_SAVE_INTERVAL;
            uint64_t loss_output_interval = 1'000'000;

            size_t sfen_read_size = SfenReader::DEFAULT_SFEN_READ_SIZE;
            size_t thread_buffer_size = SfenReader::DEFAULT_THREAD_BUFFER_SIZE;

            bool use_draw_games_in_training = true;
            bool use_draw_games_in_validation = true;
            bool skip_duplicated_positions_in_training = true;

            bool assume_quiet = false;
            bool smart_fen_skipping = false;

            double learning_rate = 1.0;

            string validation_set_file_name;
            string seed;

            std::vector<std::string> filenames;

            uint64_t num_threads;

            void enforce_constraints()
            {
                num_threads = Options["Threads"];

                if (loss_output_interval == 0)
                {
                    loss_output_interval = LEARN_RMSE_OUTPUT_INTERVAL * mini_batch_size;
                }

                // If reduction_gameply is set to 0, rand(0) will be divided by 0, so correct it to 1.
                reduction_gameply = max(reduction_gameply, 1);

                if (newbob_decay != 1.0 && !Options["SkipLoadingEval"]) {
                    // Save the current net to [EvalSaveDir]\original.
                    Eval::NNUE::save_eval("original");

                    // Set the folder above to best_nn_directory so that the trainer can
                    // resotre the network parameters from the original net file.
                    best_nn_directory =
                        Path::combine(Options["EvalSaveDir"], "original");
                }
            }
        };

        // Number of phases used for calculation such as mse
        // mini-batch size = 1M is standard, so 0.2% of that should be negligible in terms of time.
        // Since search() is performed with depth = 1 in calculation of
        // move match rate, simple comparison is not possible...
        static constexpr uint64_t sfen_for_mse_size = 2000;

        LearnerThink(const Params& prm) :
            params(prm),
            prng(prm.seed),
            sr(
                prm.filenames,
                prm.shuffle,
                SfenReaderMode::Cyclic,
                prm.num_threads,
                std::to_string(prng.next_random_seed()),
                prm.sfen_read_size,
                prm.thread_buffer_size),
            learn_loss_sum{}
        {
            save_count = 0;
            loss_output_count = 0;
            last_lr_drop = 0;
            best_loss = std::numeric_limits<double>::infinity();
            latest_loss_sum = 0.0;
            latest_loss_count = 0;
            total_done = 0;
            trials = params.newbob_num_trials;
            dir_number = 0;
        }

        void learn(uint64_t epochs);

    private:
        static void set_learning_search_limits();

        void learn_worker(Thread& th, std::atomic<uint64_t>& counter, uint64_t limit);

        void update_weights(const PSVector& psv, uint64_t epoch);

        void calc_loss(const PSVector& psv, uint64_t epoch);

        void calc_loss_worker(
            Thread& th,
            std::atomic<uint64_t>& counter,
            const PSVector& psv,
            Loss& test_loss_sum,
            atomic<double>& sum_norm,
            atomic<int>& move_accord_count
        );

        Value get_shallow_value(Position& pos);

        bool check_progress();

        // save merit function parameters to a file
        bool save(bool is_final = false);

        Params params;

        PRNG prng;

        // sfen reader
        SfenReader sr;

        uint64_t save_count;
        uint64_t loss_output_count;

        std::atomic<bool> stop_flag;

        uint64_t total_done;

        uint64_t last_lr_drop;
        double best_loss;
        double latest_loss_sum;
        uint64_t latest_loss_count;

        int trials;
        int dir_number;

        // For calculation of learning data loss
        Loss learn_loss_sum;
    };

    void LearnerThink::set_learning_search_limits()
    {
        Threads.main()->ponder = false;

        // About Search::Limits
        // Be careful because this member variable is global and affects other threads.
        auto& limits = Search::Limits;

        limits.startTime = now();

        // Make the search equivalent to the "go infinite" command. (Because it is troublesome if time management is done)
        limits.infinite = true;

        // Since PV is an obstacle when displayed, erase it.
        limits.silent = true;

        // If you use this, it will be compared with the accumulated nodes of each thread. Therefore, do not use it.
        limits.nodes = 0;

        // depth is also processed by the one passed as an argument of Learner::search().
        limits.depth = 0;
    }

    void LearnerThink::learn(uint64_t epochs)
    {
#if defined(_OPENMP)
        omp_set_num_threads((int)Options["Threads"]);
#endif

        set_learning_search_limits();

        Eval::NNUE::verify_any_net_loaded();

        const PSVector sfen_for_mse =
            params.validation_set_file_name.empty()
            ? sr.read_for_mse(sfen_for_mse_size)
            : sr.read_validation_set(
                params.validation_set_file_name,
                params.eval_limit,
                params.use_draw_games_in_validation);

        if (params.validation_set_file_name.empty()
            && sfen_for_mse.size() != sfen_for_mse_size)
        {
            auto out = sync_region_cout.new_region();
            out
                << "INFO (learn): Error reading sfen_for_mse. Read " << sfen_for_mse.size()
                << " out of " << sfen_for_mse_size << '\n';

            return;
        }

        if (params.newbob_decay != 1.0) {

            calc_loss(sfen_for_mse, 0);

            best_loss = latest_loss_sum / latest_loss_count;
            latest_loss_sum = 0.0;
            latest_loss_count = 0;

            auto out = sync_region_cout.new_region();
            out << "INFO (learn): initial loss = " << best_loss << endl;
        }

        stop_flag = false;

        for(uint64_t epoch = 1; epoch <= epochs; ++epoch)
        {
            std::atomic<uint64_t> counter{0};

            Threads.execute_with_workers([this, &counter](auto& th){
                learn_worker(th, counter, params.mini_batch_size);
            });

            total_done += params.mini_batch_size;

            Threads.wait_for_workers_finished();

            if (stop_flag)
                break;

            update_weights(sfen_for_mse, epoch);

            if (stop_flag)
                break;
        }

        Eval::NNUE::finalize_net();

        save(true);
    }

    void LearnerThink::learn_worker(Thread& th, std::atomic<uint64_t>& counter, uint64_t limit)
    {
        const auto thread_id = th.thread_idx();
        auto& pos = th.rootPos;

        Loss local_loss_sum{};
        std::vector<StateInfo, AlignedAllocator<StateInfo>> state(MAX_PLY);

        while(!stop_flag)
        {
            const auto iter = counter.fetch_add(1);
            if (iter >= limit)
                break;

            PackedSfenValue ps;

        RETRY_READ:;

            if (!sr.read_to_thread_buffer(thread_id, ps))
            {
                // If we ran out of data we stop completely
                // because there's nothing left to do.
                stop_flag = true;
                break;
            }

            if (params.eval_limit < abs(ps.score))
                goto RETRY_READ;

            if (!params.use_draw_games_in_training && ps.game_result == 0)
                goto RETRY_READ;

            // Skip over the opening phase
            if (ps.gamePly < prng.rand(params.reduction_gameply))
                goto RETRY_READ;

            StateInfo si;
            if (pos.set_from_packed_sfen(ps.sfen, &si, &th) != 0)
            {
                // Malformed sfen
                auto out = sync_region_cout.new_region();
                out << "ERROR: illigal packed sfen = " << pos.fen() << endl;
                goto RETRY_READ;
            }

            const auto rootColor = pos.side_to_move();

            // A function that adds the current `pos` and `ps`
            // to the training set.
            auto pos_add_grad = [&]() {

                // Evaluation value of deep search
                const auto deep_value = (Value)ps.score;

                const Value shallow_value = Eval::evaluate(pos);

                const auto loss = get_loss(
                    deep_value,
                    (rootColor == pos.side_to_move()) ? shallow_value : -shallow_value,
                    ps);

                local_loss_sum += loss;

                Eval::NNUE::add_example(pos, rootColor, shallow_value, ps, 1.0);
            };

            if (!pos.pseudo_legal((Move)ps.move) || !pos.legal((Move)ps.move))
            {
                goto RETRY_READ;
            }

            // We don't need to qsearch when doing smart skipping
            if (!params.assume_quiet && !params.smart_fen_skipping)
            {
                int ply = 0;
                pos.do_move((Move)ps.move, state[ply++]);

                // Evaluation value of shallow search (qsearch)
                const auto [_, pv] = Search::qsearch(pos);

                for (auto m : pv)
                {
                    pos.do_move(m, state[ply++]);
                }
            }

            if (params.smart_fen_skipping
                && (pos.capture_or_promotion((Move)ps.move)
                    || pos.checkers()))
            {
                goto RETRY_READ;
            }

            // We want to position being trained on not to be terminal
            if (MoveList<LEGAL>(pos).size() == 0)
                goto RETRY_READ;

            // Since we have reached the end phase of PV, add the slope here.
            pos_add_grad();
        }

        learn_loss_sum += local_loss_sum;
    }

    void LearnerThink::update_weights(const PSVector& psv, uint64_t epoch)
    {
        // I'm not sure this fencing is correct. But either way there
        // should be no real issues happening since
        // the read/write phases are isolated.
        atomic_thread_fence(memory_order_seq_cst);
        Eval::NNUE::update_parameters(Threads, epoch, params.verbose, params.learning_rate, get_loss);
        atomic_thread_fence(memory_order_seq_cst);

        if (++save_count * params.mini_batch_size >= params.eval_save_interval)
        {
            save_count = 0;

            const bool converged = save();
            if (converged)
            {
                stop_flag = true;
                return;
            }
        }

        if (++loss_output_count * params.mini_batch_size >= params.loss_output_interval)
        {
            loss_output_count = 0;

            // loss calculation
            calc_loss(psv, epoch);

            Eval::NNUE::check_health();
        }
    }

    void LearnerThink::calc_loss(const PSVector& psv, uint64_t epoch)
    {
        TT.new_search();
        TimePoint elapsed = now() - Search::Limits.startTime + 1;

        auto out = sync_region_cout.new_region();

        out << "\n";
        out << "PROGRESS (calc_loss): " << now_string()
             << ", " << total_done << " sfens"
             << ", " << total_done * 1000 / elapsed  << " sfens/second"
             << ", epoch " << epoch
             << endl;

        out << "  - learning rate = " << params.learning_rate << endl;

        // For calculation of verification data loss
        Loss test_loss_sum{};

        // norm for learning
        atomic<double> sum_norm{0.0};

        // The number of times the pv first move of deep
        // search matches the pv first move of search(1).
        atomic<int> move_accord_count{0};

        auto mainThread = Threads.main();
        mainThread->execute_with_worker([&out](auto& th){
            auto& pos = th.rootPos;
            StateInfo si;
            pos.set(StartFEN, false, &si, &th);
            out << "  - startpos eval = " << Eval::evaluate(pos) << endl;
        });
        mainThread->wait_for_worker_finished();

        // The number of tasks to do.
        atomic<uint64_t> counter{0};
        Threads.execute_with_workers([&](auto& th){
            calc_loss_worker(
                th,
                counter,
                psv,
                test_loss_sum,
                sum_norm,
                move_accord_count
            );
        });
        Threads.wait_for_workers_finished();

        latest_loss_sum += test_loss_sum.value();
        latest_loss_count += psv.size();

        if (psv.size() && test_loss_sum.count() > 0)
        {
            test_loss_sum.print("test", out);

            if (learn_loss_sum.count() > 0)
            {
                learn_loss_sum.print("learn", out);
            }

            out << "  - norm = " << sum_norm << endl;
            out << "  - move accuracy = " << (move_accord_count * 100.0 / psv.size()) << "%" << endl;
        }
        else
        {
            out << "ERROR: psv.size() = " << psv.size() << " ,  done = " << test_loss_sum.count() << endl;
        }

        learn_loss_sum.reset();
    }

    void LearnerThink::calc_loss_worker(
        Thread& th,
        std::atomic<uint64_t>& counter,
        const PSVector& psv,
        Loss& test_loss_sum,
        atomic<double>& sum_norm,
        atomic<int>& move_accord_count
    )
    {
        Loss local_loss_sum{};
        auto& pos = th.rootPos;

        for(;;)
        {
            const auto task_id = counter.fetch_add(1);
            if (task_id >= psv.size())
            {
                break;
            }

            const auto& ps = psv[task_id];

            StateInfo si;
            if (pos.set_from_packed_sfen(ps.sfen, &si, &th) != 0)
            {
                cout << "Error! : illegal packed sfen " << pos.fen() << endl;
                continue;
            }

            const Value shallow_value = get_shallow_value(pos);

            // Evaluation value of deep search
            const auto deep_value = (Value)ps.score;

            const auto loss = get_loss(
                deep_value,
                shallow_value,
                ps);

            local_loss_sum += loss;
            sum_norm += (double)abs(shallow_value);

            // Determine if the teacher's move and the score of the shallow search match
            const auto [value, pv] = Search::search(pos, 1);
            if (pv.size() > 0 && (uint16_t)pv[0] == ps.move)
                move_accord_count.fetch_add(1, std::memory_order_relaxed);
        }

        test_loss_sum += local_loss_sum;
    }

    Value LearnerThink::get_shallow_value(Position& pos)
    {
        // Evaluation value for shallow search
        // The value of evaluate() may be used, but when calculating loss, learn_cross_entropy and
        // Use qsearch() because it is difficult to compare the values.
        // EvalHash has been disabled in advance. (If not, the same value will be returned every time)
        const auto [_, pv] = Search::qsearch(pos);

        const auto rootColor = pos.side_to_move();

        std::vector<StateInfo, AlignedAllocator<StateInfo>> states(pv.size());
        for (size_t i = 0; i < pv.size(); ++i)
        {
            pos.do_move(pv[i], states[i]);
        }

        const Value shallow_value =
            (rootColor == pos.side_to_move())
            ? Eval::evaluate(pos)
            : -Eval::evaluate(pos);

        for (auto it = pv.rbegin(); it != pv.rend(); ++it)
            pos.undo_move(*it);

        return shallow_value;
    }

    bool LearnerThink::check_progress()
    {
        auto out = sync_region_cout.new_region();

        const double latest_loss = latest_loss_sum / latest_loss_count;
        bool converged = false;
        latest_loss_sum = 0.0;
        latest_loss_count = 0;

        auto drop_lr = [&]() {
            last_lr_drop = total_done;

            out
                << "  - reducing learning rate from " << params.learning_rate
                << " to " << (params.learning_rate * params.newbob_decay)
                << " (" << trials << " more trials)" << endl;

            params.learning_rate *= params.newbob_decay;
        };

        auto accept = [&]() {
            out << "  - loss = " << latest_loss << " < best (" << best_loss << "), accepted" << endl;

            best_loss = latest_loss;
            trials = params.newbob_num_trials;
        };

        auto reject = [&]() {
            out << "  - loss = " << latest_loss << " >= best (" << best_loss << "), rejected" << endl;

            --trials;
            if (trials > 0)
            {
                drop_lr();
                return false;
            }
            else
            {
                return true;
            }
        };

        out << "INFO (learning_rate):" << endl;

        if (params.auto_lr_drop)
        {
            accept();

            if (total_done >= last_lr_drop + params.auto_lr_drop)
            {
                drop_lr();
            }
        }
        else if (latest_loss < best_loss)
        {
            accept();
        }
        else
        {
            converged = reject();
        }

        if (converged)
        {
            out << "  - converged" << endl;
        }

        return converged;
    }

    // Write evaluation function file.
    bool LearnerThink::save(bool is_final)
    {
        // Each time you save, change the extension part of the file name like "0","1","2",..
        // (Because I want to compare the winning rate for each evaluation function parameter later)

        bool converged = false;

        if (params.save_only_once)
        {
            // When EVAL_SAVE_ONLY_ONCE is defined,
            // Do not dig a subfolder because I want to save it only once.
            Eval::NNUE::save_eval("");
        }
        else if (is_final)
        {
            Eval::NNUE::save_eval("final");
            converged = true;
        }
        else
        {
            // TODO: consider naming the output directory by epoch.
            const std::string dir_name = std::to_string(dir_number++);
            Eval::NNUE::save_eval(dir_name);

            if (params.newbob_decay != 1.0 && latest_loss_count > 0)
            {
                converged = check_progress();
                params.best_nn_directory = Path::combine((std::string)Options["EvalSaveDir"], dir_name);
            }
        }

        return converged;
    }

    // Learning from the generated game record
    void learn(istringstream& is)
    {
        LearnerThink::Params params;

        // Number of epochs
        uint64_t epochs = std::numeric_limits<uint64_t>::max();

        // Game file storage folder (get game file with relative path from here)
        string base_dir;
        string target_dir;

        uint64_t nn_batch_size = 1000;
        string nn_options;

        auto out = sync_region_cout.new_region();

        // Assume the filenames are staggered.
        while (true)
        {
            string option;
            is >> option;

            if (option == "")
                break;

            // specify the number of phases of mini-batch
            if (option == "bat")
            {
                is >> params.mini_batch_size;
                params.mini_batch_size *= 10000; // Unit is ten thousand
            }

            // Specify the folder in which the game record is stored and make it the rooting target.
            else if (option == "targetdir") is >> target_dir;
            else if (option == "targetfile")
            {
                std::string filename;
                is >> filename;
                params.filenames.push_back(filename);
            }

            // Specify the number of loops
            else if (option == "epochs") is >> epochs;

            // Game file storage folder (get game file with relative path from here)
            else if (option == "basedir") is >> base_dir;

            // Mini batch size
            else if (option == "batchsize") is >> params.mini_batch_size;

            // learning rate
            else if (option == "lr") is >> params.learning_rate;

            // Accept also the old option name.
            else if (option == "use_draw_in_training"
                  || option == "use_draw_games_in_training")
                is >> params.use_draw_games_in_training;

            // Accept also the old option name.
            else if (option == "use_draw_in_validation"
                  || option == "use_draw_games_in_validation")
                is >> params.use_draw_games_in_validation;

            // Accept also the old option name.
            else if (option == "use_hash_in_training"
                  || option == "skip_duplicated_positions_in_training")
                is >> params.skip_duplicated_positions_in_training;

            else if (option == "winning_probability_coefficient")
                is >> winning_probability_coefficient;

            // Using WDL with win rate model instead of sigmoid
            else if (option == "use_wdl") is >> use_wdl;


            // LAMBDA
            else if (option == "lambda") is >> elmo_lambda_low;
            else if (option == "lambda2") is >> elmo_lambda_high;
            else if (option == "lambda_limit") is >> elmo_lambda_limit;
            else if (option == "max_grad") is >> max_grad;

            else if (option == "reduction_gameply") is >> params.reduction_gameply;

            else if (option == "eval_limit") is >> params.eval_limit;
            else if (option == "save_only_once") params.save_only_once = true;
            else if (option == "no_shuffle") params.shuffle = false;

            else if (option == "nn_batch_size") is >> nn_batch_size;
            else if (option == "newbob_decay") is >> params.newbob_decay;
            else if (option == "newbob_num_trials") is >> params.newbob_num_trials;
            else if (option == "nn_options") is >> nn_options;
            else if (option == "auto_lr_drop") is >> params.auto_lr_drop;

            else if (option == "eval_save_interval") is >> params.eval_save_interval;
            else if (option == "loss_output_interval") is >> params.loss_output_interval;
            else if (option == "validation_set_file_name") is >> params.validation_set_file_name;

            else if (option == "src_score_min_value") is >> src_score_min_value;
            else if (option == "src_score_max_value") is >> src_score_max_value;
            else if (option == "dest_score_min_value") is >> dest_score_min_value;
            else if (option == "dest_score_max_value") is >> dest_score_max_value;

            else if (option == "sfen_read_size") is >> params.sfen_read_size;
            else if (option == "thread_buffer_size") is >> params.thread_buffer_size;

            else if (option == "seed") is >> params.seed;
            else if (option == "set_recommended_uci_options")
            {
                UCI::setoption("Use NNUE", "pure");
                UCI::setoption("MultiPV", "1");
                UCI::setoption("Contempt", "0");
                UCI::setoption("Skill Level", "20");
                UCI::setoption("UCI_Chess960", "false");
                UCI::setoption("UCI_AnalyseMode", "false");
                UCI::setoption("UCI_LimitStrength", "false");
                UCI::setoption("PruneAtShallowDepth", "false");
                UCI::setoption("EnableTranspositionTable", "false");
            }
            else if (option == "verbose") params.verbose = true;
            else if (option == "assume_quiet") params.assume_quiet = true;
            else if (option == "smart_fen_skipping") params.smart_fen_skipping = true;
            else
            {
                out << "INFO: Unknown option: " << option << ". Ignoring.\n";
            }
        }

        out << "INFO: Executing learn command\n";

        // Issue a warning if OpenMP is disabled.
#if !defined(_OPENMP)
        out << "WARNING: OpenMP disabled." << endl;
#endif

        params.enforce_constraints();

        // Right now we only have the individual files.
        // We need to apply base_dir here
        if (!target_dir.empty())
        {
            append_files_from_dir(params.filenames, base_dir, target_dir);
        }
        rebase_files(params.filenames, base_dir);

        out << "INFO: Input files:\n";
        for (auto s : params.filenames)
            out << "  - " << s << '\n';

        out << "INFO: Parameters:\n";
        if (!params.validation_set_file_name.empty())
        {
            out << "  - validation set           : " << params.validation_set_file_name << endl;
        }

        out << "  - epochs                   : " << epochs << endl;
        out << "  - epochs * minibatch size  : " << epochs * params.mini_batch_size << endl;
        out << "  - eval_limit               : " << params.eval_limit << endl;
        out << "  - save_only_once           : " << (params.save_only_once ? "true" : "false") << endl;
        out << "  - shuffle on read          : " << (params.shuffle ? "true" : "false") << endl;

        out << "  - Loss Function            : " << LOSS_FUNCTION << endl;
        out << "  - minibatch size           : " << params.mini_batch_size << endl;

        out << "  - nn_batch_size            : " << nn_batch_size << endl;
        out << "  - nn_options               : " << nn_options << endl;

        out << "  - learning rate            : " << params.learning_rate << endl;
        out << "  - use draws in training    : " << params.use_draw_games_in_training << endl;
        out << "  - use draws in validation  : " << params.use_draw_games_in_validation << endl;
        out << "  - skip repeated positions  : " << params.skip_duplicated_positions_in_training << endl;

        out << "  - winning prob coeff       : " << winning_probability_coefficient << endl;
        out << "  - use_wdl                  : " << use_wdl << endl;

        out << "  - src_score_min_value      : " << src_score_min_value << endl;
        out << "  - src_score_max_value      : " << src_score_max_value << endl;
        out << "  - dest_score_min_value     : " << dest_score_min_value << endl;
        out << "  - dest_score_max_value     : " << dest_score_max_value << endl;

        out << "  - reduction_gameply        : " << params.reduction_gameply << endl;

        out << "  - elmo_lambda_low          : " << elmo_lambda_low << endl;
        out << "  - elmo_lambda_high         : " << elmo_lambda_high << endl;
        out << "  - elmo_lambda_limit        : " << elmo_lambda_limit << endl;
        out << "  - max_grad                 : " << max_grad << endl;
        out << "  - eval_save_interval       : " << params.eval_save_interval << " sfens" << endl;
        out << "  - loss_output_interval     : " << params.loss_output_interval << " sfens" << endl;

        out << "  - sfen_read_size           : " << params.sfen_read_size << endl;
        out << "  - thread_buffer_size       : " << params.thread_buffer_size << endl;

        out << "  - seed                     : " << params.seed << endl;
        out << "  - verbose                  : " << (params.verbose ? "true" : "false") << endl;

        if (params.auto_lr_drop) {
            out << "  - learning rate scheduling : every " << params.auto_lr_drop << " sfens" << endl;
        }
        else if (params.newbob_decay != 1.0) {
            out << "  - learning rate scheduling : newbob with decay" << endl;
            out << "  - newbob_decay             : " << params.newbob_decay << endl;
            out << "  - newbob_num_trials        : " << params.newbob_num_trials << endl;
        }
        else {
            out << "  - learning rate scheduling : fixed learning rate" << endl;
        }

        out << endl;

        out << "INFO: Started initialization." << endl;

        Eval::NNUE::initialize_training(params.seed, out);
        Eval::NNUE::set_batch_size(nn_batch_size);
        Eval::NNUE::set_options(nn_options);

        LearnerThink learn_think(params);

        out << "Finished initialization." << endl;

        out.unlock();

        // Start learning.
        learn_think.learn(epochs);
    }

} // namespace Learner