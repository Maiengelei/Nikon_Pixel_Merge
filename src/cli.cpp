#include "job.hpp"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;
using namespace npm;
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
void usage(std::ostream& out) {
    out << "用法：\n  nikon-merge scan <文件夹或 NEF> ...\n"
           "  nikon-merge merge <文件夹> --group <序列 ID> --ca on|off --output <新文件.nefx>\n"
           "只有一个序列时可省略 --group。默认开启色差矫正。不会覆盖已有文件。\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc==1 || std::string(argv[1])=="--help") { usage(std::cout); return 0; }
        std::string command=argv[1];
        if (command!="scan" && command!="merge") { usage(std::cerr); return 2; }
        fs::path destination;
        std::vector<fs::path> inputs;
        std::string id;
        bool ca=true;
        for (int i=2;i<argc;i++) {
            std::string arg=argv[i];
            if (arg=="--group" || arg=="--ca" || arg=="--output") {
                if (command!="merge" || ++i>=argc) throw std::invalid_argument("选项缺少参数或不可用于扫描：" + arg);
                std::string value=argv[i];
                if (arg=="--group") id=value;
                else if (arg=="--output") destination=value;
                else if (value=="on" || value=="off") ca=value=="on";
                else throw std::invalid_argument("--ca 只接受 on 或 off");
            } else if (arg.rfind("--",0)==0) throw std::invalid_argument("未知选项：" + arg);
            else inputs.emplace_back(arg);
        }
        if (inputs.empty()) throw std::invalid_argument("需要至少一个输入文件夹或 NEF");
        auto result=scan_groups(inputs);
        for (const auto& i : result.issues) std::cerr << i.path.string() << ": " << i.message << "\n";
        if (command=="scan") {
            for (const auto& g: result.groups) {
                auto problem=group_problem(g);
                std::cout << g.id << "\t" << g.model << "\t" << g.frames.size() << "/" << g.expected
                          << "\t" << (problem.empty()?"可合并":problem) << "\t" << g.frames.front().path.string() << "\n";
            }
            std::cerr << result.groups.size() << " 组；" << result.ordinary_nefs << " 张非像素位移 NEF；"
                      << result.issues.size() << " 个读取问题\n";
            return result.groups.empty() && !result.issues.empty() ? 1 : 0;
        }
        if (destination.empty()) throw std::invalid_argument("请用 --output 指定一个新的 .nefx 文件");
        const Group* selected=nullptr;
        if (id.empty() && result.groups.size()==1) selected=&result.groups.front();
        else for (const auto& g: result.groups) if (g.id==id) selected=&g;
        if (!selected) throw std::invalid_argument("未选中唯一序列；请先 scan，再用 --group 指定序列 ID");
        std::signal(SIGINT, stop);
        int percent=-1;
        auto start=std::chrono::steady_clock::now();
        export_group(*selected,destination,ca,[&](double p, const std::string& stage){
            if (stopped) throw Cancelled{};
            int now=static_cast<int>(100*p);
            if (now!=percent) { std::cerr << now << "% " << stage << "\n"; percent=now; }
        });
        std::cout << destination.string() << "  (" << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() << " s)\n";
        return 0;
    } catch (const Cancelled& e) { std::cerr << e.what() << "\n"; return 130; }
      catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
