#include "training_log.h"

#include <fstream>
#include <iterator>
#include <sstream>

void training_log_append(const std::string & topic, const std::string & content, const std::string & log_path) {
    std::string block = "### " + topic + "\n" + content + "\n\n";

    {
        std::ifstream existing(log_path);
        if (existing.is_open()) {
            std::string all((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (all.find(block) != std::string::npos) {
                return;
            }
        }
    }

    std::ofstream out(log_path, std::ios::app);
    if (out.is_open()) {
        out << block;
    }
}
