#ifndef PLATEAU_LR_SCHEDULER_HPP
#define PLATEAU_LR_SCHEDULER_HPP

#include <limits>
#include <string>
#include <iostream>

class PlateauLRScheduler {
public:
    // mode: "min" or "max"
    // factor: Multiplicative factor by which the LR is reduced on plateau
    // patience: Number of steps with no improvement before reducing LR
    // cooldown: Number of steps to wait after a reduction before next potential reduction
    // min_lr: Lower bound on the learning rate
    PlateauLRScheduler(double initial_lr, double factor, int patience,
                       const std::string &mode = "min", int cooldown = 0, double min_lr = 0.0)
            : lr_(initial_lr), factor_(factor), patience_(patience), mode_(mode),
              cooldown_(cooldown), min_lr_(min_lr),
              best_(mode == "min" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity()),
              num_bad_steps_(0), cooldown_counter_(0) {}

    double get_lr() const {
        return lr_;
    }

    // Call at each step (epoch) with the current monitored value (e.g., training or validation loss)
    void step(double current_value) {
        bool is_improved = (mode_ == "min") ? (current_value < best_) : (current_value > best_);
        if (is_improved) {
            best_ = current_value;
            num_bad_steps_ = 0;
            cooldown_counter_ = 0;
        } else {
            if (cooldown_counter_ > 0) {
                cooldown_counter_--;
            } else {
                num_bad_steps_++;
                if (num_bad_steps_ > patience_) {
                    reduce_lr();
                    cooldown_counter_ = cooldown_;
                    num_bad_steps_ = 0;
                }
            }
        }
    }

private:
    void reduce_lr() {
        double new_lr = lr_ * factor_;
        if (new_lr < min_lr_) {
            new_lr = min_lr_;
        }
        if (new_lr < lr_) {
            lr_ = new_lr;
            std::cout << "Reducing learning rate to " << lr_ << "\n";
        }
    }

    double lr_;
    double factor_;
    int patience_;
    std::string mode_;
    int cooldown_;
    double min_lr_;

    double best_;
    int num_bad_steps_;
    int cooldown_counter_;
};

#endif // PLATEAU_LR_SCHEDULER_HPP
