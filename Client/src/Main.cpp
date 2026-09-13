#ifndef _DEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif // !_DEBUG


#include "view/ImGuiView.h"
#include "presenter/presenter.h"
#include "model/Config.h"
#include <omp.h>
#include "tracker/TrackerFactory.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"


#include "model/UpdateChecker.h"

#include <atomic>
#include <exception>
#include <thread>



int main(int argc, char *argv[])
{
    std::shared_ptr<spdlog::logger> logger;
    try
    {
        logger = spdlog::basic_logger_mt("aitrack", "log.txt", true);
        logger->flush_on(spdlog::level::info);
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log init failed: " << ex.what() << std::endl;
    }


    logger->info(" ----------  AITRACK LOG   ----------");

    auto conf_mgr = std::make_unique<ConfigMgr>("./prefs.ini");
    logger->info("Created/Found prefs.ini");

    auto state = conf_mgr->getConfig();
    if (state.onnx_set_env_threads) {
        std::wstring ws = std::to_wstring(state.onnx_env_threads);
        SetEnvironmentVariable(LPWSTR(L"OMP_NUM_THREADS"), ws.c_str());
    }
    if (state.onnx_set_num_threads) {
        omp_set_num_threads(state.onnx_num_threads);
    }
    if (state.onnx_set_dynamic) {
        omp_set_dynamic(state.onnx_dynamic);
    }

    ImGuiView view;

    auto t_factory = std::make_unique<TrackerFactory>("./models/");

    std::unique_ptr<Presenter> presenter;
    std::exception_ptr startup_error;
    std::thread startup_thread([&]() {
        try
        {
            presenter = std::make_unique<Presenter>((IView&)view, std::move(t_factory), std::move(conf_mgr));
        }
        catch (...)
        {
            startup_error = std::current_exception();
        }
        view.set_startup_complete();
    });

    const int result = view.run();
    startup_thread.join();
    if (startup_error)
        std::rethrow_exception(startup_error);
    logger->info("App initialized");
    return result;
}
