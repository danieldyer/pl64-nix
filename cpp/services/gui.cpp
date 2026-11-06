#include "pl64/control.hpp"
#include "pl64/ipc.hpp"
#include "system_generated.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace {
using pl64::Json;
constexpr ImVec4 mint{.35f, .84f, .68f, 1}, muted{.55f, .61f, .68f, 1}, amber{.97f, .73f, .36f, 1};
struct Preview {
    std::shared_ptr<pl64::Frame> lease;
    Json detail;
};
struct Model {
    std::mutex mutex;
    std::map<std::string, Json> status;
    std::deque<std::pair<std::string, Json>> commands;
    std::deque<std::string> events;
    std::map<std::string, std::string> command_errors;
    std::map<unsigned, std::shared_ptr<Preview>> previews;
    std::filesystem::path root = "/run";
    std::uint64_t revision{};
    void command(const std::string &service, Json message) {
        std::lock_guard lock(mutex);
        if (commands.size() < 32)
            commands.emplace_back(service, std::move(message));
    }
    void work(std::stop_token stop) {
        std::unique_ptr<pl64::Subscription> preview;
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            std::deque<std::pair<std::string, Json>> pending;
            {
                std::lock_guard lock(mutex);
                pending.swap(commands);
            }
            for (const auto &[service, message] : pending) {
                std::string result;
                std::string error;
                try {
                    const auto reply =
                        pl64::request(root / ("pl64-" + service) / "control.sock", message, 500);
                    result =
                        service + " · " + message.value("op", std::string()) + ": " +
                        (reply.value("ok", false) ? "accepted"
                                                  : reply.value("error", std::string("failed")));
                    if (!reply.value("ok", false))
                        error = reply.value("error", std::string("Command failed"));
                } catch (const std::exception &e) {
                    result = service + ": " + e.what();
                    error = e.what();
                }
                std::lock_guard lock(mutex);
                command_errors[service] = error;
                events.push_front(result);
                while (events.size() > 100)
                    events.pop_back();
            }
            if (std::chrono::steady_clock::now() >= next) {
                for (const auto *name : {"radar", "ins", "sar", "recorder"}) {
                    Json reply;
                    try {
                        reply = pl64::request(root / ("pl64-" + std::string(name)) / "control.sock",
                                              {{"op", "status"}}, 150);
                        reply["connected"] = true;
                        reply["received_monotonic_ns"] = pl64::monotonic_ns();
                    } catch (const std::exception &e) {
                        reply = {{"connected", false}, {"state", "Offline"}, {"error", e.what()}};
                    }
                    std::lock_guard lock(mutex);
                    status[name] = std::move(reply);
                    ++revision;
                }
                next = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                if (!preview)
                    try {
                        preview = std::make_unique<pl64::Subscription>(
                            root / "pl64-radar/preview.sock", 100);
                    } catch (...) {
                    }
            }
            if (preview)
                try {
                    for (unsigned n = 0; n < 8; ++n) {
                        auto frame = preview->receive();
                        if (!frame)
                            break;
                        flatbuffers::Verifier verify(frame->bytes().data(), frame->bytes().size());
                        if (!pl64::system::fb::VerifyEventBuffer(verify))
                            throw std::runtime_error("Invalid preview FlatBuffer");
                        const auto *event = pl64::system::fb::GetEvent(frame->bytes().data());
                        if (event->kind() != pl64::system::fb::Kind::Preview || !event->pixels() ||
                            !event->detail_json() || !event->width() || !event->height() ||
                            std::uint64_t(event->width()) * event->height() !=
                                event->pixels()->size())
                            continue;
                        auto value = std::make_shared<Preview>();
                        value->detail = Json::parse(event->detail_json()->str());
                        value->lease = std::move(frame);
                        std::lock_guard lock(mutex);
                        previews[value->detail.value("channel", 0U)] = std::move(value);
                    }
                } catch (...) {
                    preview.reset();
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};
double number(const Json &value, const char *key,
              double fallback = std::numeric_limits<double>::quiet_NaN()) {
    const auto at = value.find(key);
    return at != value.end() && at->is_number() ? at->get<double>() : fallback;
}
std::string text(const Json &value, const char *key, const char *fallback = "—") {
    const auto at = value.find(key);
    return at != value.end() && at->is_string() ? at->get<std::string>() : fallback;
}
void label(const char *name, const std::string &value) {
    ImGui::TextColored(muted, "%s", name);
    ImGui::SameLine(150);
    ImGui::TextUnformatted(value.c_str());
}
void metric(const char *name, double value, const char *units = "", int precision = 2) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    ImGui::TableNextColumn();
    if (std::isfinite(value))
        ImGui::Text("%.*f %s", precision, value, units);
    else
        ImGui::TextDisabled("Unavailable");
}
void json_tree(const Json &object) {
    if (!object.is_object()) {
        ImGui::TextWrapped("%s", object.dump().c_str());
        return;
    }
    for (const auto &[key, value] : object.items()) {
        if (value.is_structured()) {
            if (ImGui::TreeNode(key.c_str())) {
                json_tree(value);
                ImGui::TreePop();
            }
        } else {
            ImGui::TextColored(muted, "%s", key.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", value.dump().c_str());
        }
    }
}
void state_badge(const Json &value) {
    const auto state = text(value, "state", "Offline");
    const bool live = value.value("connected", false);
    ImGui::TextColored(!live ? muted : state == "Fault" ? amber : mint, "%s", state.c_str());
}
void navigation_badge(const Json &value) {
    if (!value.value("connected", false)) {
        state_badge(value);
        return;
    }
    const auto stage = text(value, "stage", "");
    if (stage == "Simulator")
        ImGui::TextColored(amber, "Simulation");
    else if (stage == "navigating")
        ImGui::TextColored(mint, "Navigating");
    else if (stage == "degraded")
        ImGui::TextColored(amber, "Degraded");
    else
        ImGui::TextColored(amber, "Aligning");
}
void theme() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowRounding = 5;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.PopupRounding = 5;
    s.WindowPadding = {14, 12};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {9, 7};
    s.CellPadding = {8, 6};
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.ScrollbarSize = 12;
    auto *c = s.Colors;
    c[ImGuiCol_WindowBg] = {.065f, .078f, .097f, 1};
    c[ImGuiCol_ChildBg] = {.065f, .078f, .097f, 1};
    c[ImGuiCol_TitleBg] = {.09f, .11f, .135f, 1};
    c[ImGuiCol_TitleBgActive] = {.11f, .14f, .17f, 1};
    c[ImGuiCol_Header] = {.14f, .23f, .27f, 1};
    c[ImGuiCol_HeaderHovered] = {.18f, .32f, .36f, 1};
    c[ImGuiCol_HeaderActive] = {.2f, .38f, .4f, 1};
    c[ImGuiCol_Button] = {.15f, .27f, .29f, 1};
    c[ImGuiCol_ButtonHovered] = {.19f, .38f, .37f, 1};
    c[ImGuiCol_ButtonActive] = {.23f, .46f, .42f, 1};
    c[ImGuiCol_CheckMark] = mint;
    c[ImGuiCol_SliderGrab] = mint;
    c[ImGuiCol_PlotLines] = mint;
    c[ImGuiCol_Text] = {.88f, .91f, .94f, 1};
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_Border] = {.17f, .2f, .24f, .8f};
    c[ImGuiCol_FrameBg] = {.1f, .13f, .16f, 1};
    c[ImGuiCol_Tab] = {.1f, .15f, .18f, 1};
    c[ImGuiCol_TabSelected] = {.15f, .26f, .29f, 1};
}
void layout(ImGuiID dock, const ImVec2 &size, float scale) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, size);
    auto center = dock;
    const auto overview = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Up, std::min(.25f, 120 * scale / size.y), nullptr, &center);
    auto left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,
                                            size.x / scale < 1400 ? .34f : .28f, nullptr, &center);
    auto bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, .30f, nullptr, &center);
    auto left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, .38f, nullptr, &left);
    ImGui::DockBuilderDockWindow("Overview", overview);
    ImGui::DockBuilderDockWindow("Capture", left);
    ImGui::DockBuilderDockWindow("Recording", left_bottom);
    ImGui::DockBuilderDockWindow("Range · Doppler", center);
    ImGui::DockBuilderDockWindow("Channels", bottom);
    ImGui::DockBuilderDockWindow("Navigation", left_bottom);
    ImGui::DockBuilderDockWindow("SAR / ISAR", left_bottom);
    ImGui::DockBuilderDockWindow("Events", left_bottom);
    ImGui::DockBuilderDockWindow("Service details", left_bottom);
    ImGui::DockBuilderFinish(dock);
}
void screenshot(const std::filesystem::path &path, GLFWwindow *window) {
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    std::vector<unsigned char> pixels(std::size_t(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (int row = height; row-- > 0;)
        out.write(reinterpret_cast<const char *>(pixels.data() + std::size_t(row) * width * 3),
                  std::streamsize(width) * 3);
}
} // namespace
int main(int argc, char **argv) try {
    Model model;
    std::filesystem::path image;
    unsigned frames = 0;
    int width = 1500, height = 960;
    float requested_scale = 0;
    bool force_layout = false;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") {
            std::cout << "pl64-control [--runtime-dir DIR] [--ui-scale 1.5|2] [--reset-layout]\n"
                         "View > UI scale changes and saves scaling. Session > Exit or Ctrl+Q "
                         "closes the GUI.\n";
            return 0;
        }
        if (key == "--reset-layout") {
            force_layout = true;
            continue;
        }
        if (++i == argc)
            throw std::invalid_argument("Missing argument");
        if (key == "--runtime-dir")
            model.root = argv[i];
        else if (key == "--screenshot")
            image = argv[i];
        else if (key == "--frames")
            frames = std::stoul(argv[i]);
        else if (key == "--width")
            width = std::stoi(argv[i]);
        else if (key == "--height")
            height = std::stoi(argv[i]);
        else if (key == "--ui-scale")
            requested_scale = std::stof(argv[i]);
        else
            throw std::invalid_argument("Unknown argument");
    }
    if (width < 800 || height < 600 || requested_scale < 0 || requested_scale > 2.5f)
        throw std::invalid_argument("Invalid window size or UI scale");
    glfwSetErrorCallback([](int, const char *message) { std::cerr << message << '\n'; });
    if (!glfwInit())
        throw std::runtime_error("Cannot initialize GLFW");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    if (!image.empty())
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GL_FALSE);
#endif
    if (!image.empty())
        glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
    auto *window = glfwCreateWindow(width, height, "PL64 · Capture & navigation", nullptr, nullptr);
    if (!window)
        throw std::runtime_error("Cannot create OpenGL window");
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    std::filesystem::path config;
    if (const auto *env = std::getenv("XDG_CONFIG_HOME"))
        config = env;
    else if (const auto *home = std::getenv("HOME"))
        config = std::filesystem::path(home) / ".config";
    else
        config = std::filesystem::temp_directory_path();
    config /= "pl64";
    std::filesystem::create_directories(config);
    const auto ini = (config / "control.ini").string();
    io.IniFilename = ini.c_str();
    const auto preferences = config / "control.json";
    float ui_scale = 1.5f;
    try {
        std::ifstream file(preferences);
        if (file) {
            Json value;
            file >> value;
            ui_scale = value.value("ui_scale", 1.5f);
        }
    } catch (...) {
    }
    if (requested_scale > 0)
        ui_scale = requested_scale;
    ui_scale = std::clamp(ui_scale, 1.f, 2.5f);
    theme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    std::jthread worker([&](std::stop_token stop) { model.work(stop); });
    bool reset_layout = force_layout || !std::filesystem::exists(ini), hold = false,
         auto_scale = true, show_details = false, show_events = false, timing_details = false;
    float applied_scale = 0;
    unsigned channel = 0, frame_number = 0;
    float color_low = -80, color_high = 0;
    std::array<char, 65536> settings{};
    bool settings_loaded = false;
    std::string settings_error;
    std::deque<double> history_time, history_headroom;
    std::uint64_t last_revision = 0;
    GLuint preview_texture = 0;
    std::uint64_t texture_sequence = 0, texture_stream = 0;
    float texture_low = 0, texture_high = 0;
    std::string scaled_capture;
    std::vector<ImU32> rgba;
    while (!glfwWindowShouldClose(window)) {
        if (applied_scale != ui_scale) {
            theme();
            ImGui::GetStyle().ScaleAllSizes(ui_scale);
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            io.Fonts->Clear();
            ImFontConfig font;
            font.SizePixels = 16 * ui_scale;
#ifdef PL64_FONT_PATH
            static constexpr ImWchar glyphs[] = {0x20, 0xff, 0x300, 0x3ff, 0x2000, 0x206f, 0};
            if (!io.Fonts->AddFontFromFileTTF(PL64_FONT_PATH, 16 * ui_scale, &font, glyphs))
                io.Fonts->AddFontDefault(&font);
#else
            io.Fonts->AddFontDefault(&font);
#endif
            if (applied_scale > 0) {
                reset_layout = true;
                std::ofstream file(preferences);
                file << Json{{"ui_scale", ui_scale}}.dump(2) << '\n';
            }
            applied_scale = ui_scale;
        }
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        std::map<std::string, Json> status;
        std::map<unsigned, std::shared_ptr<Preview>> previews;
        std::deque<std::string> events;
        std::map<std::string, std::string> command_errors;
        std::uint64_t revision;
        {
            std::lock_guard lock(model.mutex);
            status = model.status;
            previews = model.previews;
            events = model.events;
            command_errors = model.command_errors;
            revision = model.revision;
        }
        const auto radar = status.contains("radar") ? status["radar"] : Json::object();
        const auto nav = status.contains("ins") ? status["ins"] : Json::object();
        const auto sar = status.contains("sar") ? status["sar"] : Json::object();
        const auto recorder = status.contains("recorder") ? status["recorder"] : Json::object();
        if (radar.contains("channels") && !radar["channels"].empty() &&
            channel >= radar["channels"].size()) {
            channel = 0;
            model.command("radar", {{"op", "preview"}, {"channel", 0}});
        }
        if (revision != last_revision) {
            history_time.push_back(double(pl64::monotonic_ns()) * 1e-9);
            history_headroom.push_back(number(radar, "recent_headroom"));
            while (history_time.size() > 600) {
                history_time.pop_front();
                history_headroom.pop_front();
            }
            last_revision = revision;
        }
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Session")) {
                if (ImGui::MenuItem("Exit", "Ctrl+Q"))
                    glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Reset layout"))
                    reset_layout = true;
                ImGui::MenuItem("Channel timing", nullptr, &timing_details);
                ImGui::MenuItem("Events", nullptr, &show_events);
                ImGui::MenuItem("Service details", nullptr, &show_details);
                if (ImGui::BeginMenu("UI scale")) {
                    for (const float scale : {1.f, 1.25f, 1.5f, 1.75f, 2.f}) {
                        const auto title = std::to_string(int(scale * 100)) + "%";
                        if (ImGui::MenuItem(title.c_str(), nullptr, ui_scale == scale))
                            ui_scale = scale;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::GetWindowWidth() > 1000) {
                ImGui::SameLine(ImGui::GetWindowWidth() - 370 * ui_scale);
                ImGui::TextColored(muted, "PL64  /  Capture & navigation");
            }
            ImGui::EndMainMenuBar();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q))
            glfwSetWindowShouldClose(window, true);
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_T))
            timing_details = !timing_details;
        if (!io.WantTextInput)
            for (unsigned key = 0; key < 9; ++key)
                if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + key)) && radar.contains("channels") &&
                    key < radar["channels"].size()) {
                    channel = key;
                    model.command("radar", {{"op", "preview"}, {"channel", channel}});
                }
        const auto dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (reset_layout) {
            layout(dock, ImGui::GetMainViewport()->WorkSize, ui_scale);
            reset_layout = false;
        }
        ImGui::Begin("Overview", nullptr, ImGuiWindowFlags_NoScrollbar);
        if (ImGui::BeginTable("system_overview", 4, ImGuiTableFlags_SizingStretchSame)) {
            for (const auto &[name, window_name, value] :
                 {std::tuple{"DVB-T", "Capture", radar},
                  std::tuple{"Navigation", "Navigation", nav},
                  std::tuple{"Recording", "Recording", recorder},
                  std::tuple{"SAR / ISAR", "SAR / ISAR", sar}}) {
                ImGui::TableNextColumn();
                if (ImGui::Selectable(name))
                    ImGui::SetWindowFocus(window_name);
                if (std::string_view(name) == "Navigation")
                    navigation_badge(value);
                else
                    state_badge(value);
                if (std::string_view(name) == "DVB-T" && value.value("connected", false) &&
                    std::isfinite(number(value, "recent_headroom")))
                    ImGui::TextColored(text(value, "state") != "Running" ||
                                               number(value, "recent_headroom") >= 1
                                           ? muted
                                           : amber,
                                       "%.2f× headroom", number(value, "recent_headroom"));
                else if (std::string_view(name) == "Navigation")
                    ImGui::TextColored(muted, "%s",
                                       text(value, "stage", "Awaiting service").c_str());
                else if (std::string_view(name) == "Recording")
                    ImGui::TextColored(muted, "%s",
                                       value.value("enabled", false) ? "Logging enabled"
                                                                     : "Logging off");
                else if (std::string_view(name) == "SAR / ISAR")
                    ImGui::TextColored(muted, "Integration foundation");
            }
            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::Begin("Capture");
        state_badge(radar);
        if (radar.value("simulation", false))
            ImGui::TextColored(amber, "Simulated input");
        ImGui::TextWrapped("%s", text(radar, "message", "Waiting for capture service").c_str());
        const auto phase = text(radar, "state", "");
        const bool busy = phase != "Idle" && phase != "Fault";
        const float button_width =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
        ImGui::BeginDisabled(!radar.value("connected", false));
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Start capture", {button_width, 0}))
            model.command("radar", {{"op", "start"}});
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!busy);
        if (ImGui::Button("Stop & drain", {button_width, 0}))
            model.command("radar", {{"op", "stop"}});
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        for (const auto &error :
             {command_errors["radar"], text(radar, "configuration_warning", "")})
            if (!error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, amber);
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::PopStyleColor();
            }
        if (ImGui::BeginTable("capture_metrics", 2, ImGuiTableFlags_RowBg)) {
            metric("Input queue", number(radar, "queue_ms"), "ms", 1);
            metric("Missing", number(radar, "missing"), "samples", 0);
            metric("Host discarded", number(radar, "host_discarded"), "samples", 0);
            metric("Clock correction", number(radar, "clock_ppm"), "ppm", 3);
            if (radar.contains("rf")) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("ADC RMS/peak");
                ImGui::TableNextColumn();
                const auto rms = number(radar["rf"], "rms_dbfs"),
                           peak = number(radar["rf"], "peak_dbfs");
                if (std::isfinite(rms) && std::isfinite(peak))
                    ImGui::Text("%.1f / %.1f dBFS", rms, peak);
                else
                    ImGui::TextDisabled("Unavailable");
            }
            ImGui::EndTable();
        }
        if (ImGui::CollapsingHeader("Capture settings")) {
            if (!settings_loaded && radar.contains("settings")) {
                const auto value = radar["settings"].dump(2);
                std::snprintf(settings.data(), settings.size(), "%s", value.c_str());
                settings_loaded = true;
            }
            ImGui::BeginDisabled(busy || !radar.value("connected", false));
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * .42f);
            try {
                auto edit = Json::parse(settings.data());
                bool changed = false;
                auto real = [&](const char *label, const char *key, double step) {
                    double value = number(edit, key);
                    if (ImGui::InputDouble(label, &value, step, step * 10, "%.2f")) {
                        edit[key] = value;
                        changed = true;
                    }
                };
                real("Integration (s)", "integration", .25);
                real("Map update (s)", "update", .25);
                real("Doppler +/- Hz", "doppler", 25);
                double range = number(edit, "range_metres") / 1000;
                if (ImGui::InputDouble("Range (km)", &range, 1, 10, "%.1f")) {
                    edit["range_metres"] = range * 1000;
                    changed = true;
                }
                if (edit.contains("metadata_json") && edit["metadata_json"].contains("capture")) {
                    double gain = number(edit["metadata_json"]["capture"], "gain_db", 30);
                    if (ImGui::InputDouble("Requested gain (dB)", &gain, 1, 5, "%.1f")) {
                        edit["metadata_json"]["capture"]["gain_db"] = gain;
                        changed = true;
                    }
                }
                for (const auto &[label, key] : {std::pair{"FEC workers", "fec_workers"},
                                                 std::pair{"Frame slots", "frame_slots"}}) {
                    int value = edit.value(key, 1);
                    if (ImGui::InputInt(label, &value)) {
                        edit[key] = std::clamp(value, 1, 64);
                        changed = true;
                    }
                }
                for (const auto &[label, key] :
                     {std::pair{"Remove display DC", "remove_dc"},
                      std::pair{"Include channel I/Q", "stream_channels"}}) {
                    bool value = edit.value(key, false);
                    if (ImGui::Checkbox(label, &value)) {
                        edit[key] = value;
                        changed = true;
                    }
                }
                if (changed) {
                    const auto value = edit.dump(2);
                    std::snprintf(settings.data(), settings.size(), "%s", value.c_str());
                    settings_error.clear();
                }
            } catch (const std::exception &e) {
                settings_error = e.what();
            }
            ImGui::PopItemWidth();
            if (ImGui::TreeNode("Advanced metadata & settings")) {
                if (ImGui::InputTextMultiline("##settings", settings.data(), settings.size(),
                                              {-1, 240 * ui_scale}))
                    settings_error.clear();
                ImGui::TreePop();
            }
            if (ImGui::Button("Apply settings")) {
                try {
                    model.command(
                        "radar", {{"op", "configure"}, {"settings", Json::parse(settings.data())}});
                    settings_error.clear();
                } catch (const std::exception &e) {
                    settings_error = e.what();
                }
            }
            ImGui::EndDisabled();
            if (!settings_error.empty())
                ImGui::TextWrapped("%s", settings_error.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextWrapped("Stop capture to edit settings.");
            ImGui::PopStyleColor();
        }
        if (ImGui::CollapsingHeader("Performance history") && history_time.size() > 2 &&
            ImPlot::BeginPlot("Processing headroom", {-1, 140 * ui_scale}, ImPlotFlags_NoLegend)) {
            std::vector<double> x(history_time.begin(), history_time.end()),
                y(history_headroom.begin(), history_headroom.end());
            ImPlot::SetupAxes("Host monotonic s", "×", ImPlotAxisFlags_AutoFit,
                              ImPlotAxisFlags_AutoFit);
            ImPlot::PlotLine("Headroom", x.data(), y.data(), int(x.size()));
            ImPlot::EndPlot();
        }
        ImGui::End();
        ImGui::Begin("Channels");
        if (radar.contains("channels") && radar["channels"].is_array()) {
            if (ImGui::BeginTable("channels", timing_details ? 8 : 5,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                      ImGuiTableFlags_SizingStretchProp)) {
                if (timing_details) {
                    for (const char *title : {"Channel", "Lock", "RF MHz", "Pilot dB",
                                              "RS good / bad", "Timing rms", "Rate ppm", "Rows"})
                        ImGui::TableSetupColumn(title);
                } else {
                    for (const char *title :
                         {"Channel", "Status", "Pilot dB", "RS accepted", "Rows"})
                        ImGui::TableSetupColumn(title);
                }
                ImGui::TableHeadersRow();
                unsigned index = 0;
                for (const auto &c : radar["channels"]) {
                    ImGui::PushID(int(index));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(text(c, "name").c_str(), channel == index,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        channel = index;
                        model.command("radar", {{"op", "preview"}, {"channel", channel}});
                    }
                    ImGui::TableNextColumn();
                    const bool tracking = c.value("locked", false);
                    const bool usable = tracking && c.value("reference_calibrated", false);
                    ImGui::TextColored(usable     ? mint
                                       : tracking ? amber
                                                  : muted,
                                       "%s",
                                       usable     ? "Locked"
                                       : tracking ? "Tracking"
                                                  : "Searching");
                    if (!timing_details) {
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", number(c, "pilot_snr_db"));
                        ImGui::TableNextColumn();
                        const auto total = number(c, "rs_good") + number(c, "rs_bad");
                        if (total > 0)
                            ImGui::Text("%.1f%%", 100 * number(c, "rs_good") / total);
                        else
                            ImGui::TextDisabled("—");
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", number(c, "rows"));
                        ImGui::PopID();
                        ++index;
                        continue;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", number(c, "rf_hz") / 1e6);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", number(c, "pilot_snr_db"));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f / %.0f", number(c, "rs_good"), number(c, "rs_bad"));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", number(c, "timing_rms_samples"));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", number(c, "residual_rate_ppm"));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", number(c, "rows"));
                    ImGui::PopID();
                    ++index;
                }
                ImGui::EndTable();
            }
        } else
            ImGui::TextDisabled("Channel telemetry appears when capture starts.");
        ImGui::End();
        ImGui::Begin("Range · Doppler");
        ImGui::Checkbox("Hold", &hold);
        ImGui::SameLine();
        ImGui::Checkbox("Auto scale", &auto_scale);
        static std::shared_ptr<Preview> shown;
        if (!hold && previews.contains(channel))
            shown = previews[channel];
        if (!hold && shown) {
            const auto *event = pl64::system::fb::GetEvent(shown->lease->bytes().data());
            if (event->session() && event->session()->str() != text(radar, "capture_id", ""))
                shown.reset();
        }
        if (shown && shown->detail.value("channel", 0U) == channel) {
            const auto *value = pl64::system::fb::GetEvent(shown->lease->bytes().data());
            const auto &d = shown->detail;
            if (auto_scale) {
                const auto identity = (value->session() ? value->session()->str() : "") + "/" +
                                      std::to_string(channel);
                if (scaled_capture != identity) {
                    color_low = float(number(d, "floor_db", -80) - 1.5);
                    color_high = float(number(d, "peak_db", 0) + .25);
                    scaled_capture = identity;
                } else
                    color_high = std::max(color_high, float(number(d, "peak_db", 0) + .25));
            }
            ImGui::TextColored(muted, "%.2f s integration · capture %.2f s · %.2f dB peak",
                               number(d, "integration"), number(d, "time"), number(d, "peak_db"));
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            // Upload only when a preview or colour limit changes. ImPlot then
            // draws one textured rectangle instead of rebuilding 100k cells
            // on every GUI frame, leaving CPU/GPU time for capture.
            if (!preview_texture || texture_sequence != shown->lease->sequence ||
                texture_stream != shown->lease->stream_id || texture_low != color_low ||
                texture_high != color_high) {
                rgba.resize(value->pixels()->size());
                const auto span = std::max(color_high - color_low, 1e-6f);
                for (std::size_t i = 0; i < rgba.size(); ++i)
                    rgba[i] = ImGui::ColorConvertFloat4ToU32(ImPlot::SampleColormap(
                        std::clamp((value->pixels()->Get(i) - color_low) / span, 0.f, 1.f)));
                if (!preview_texture)
                    glGenTextures(1, &preview_texture);
                glBindTexture(GL_TEXTURE_2D, preview_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, int(value->width()), int(value->height()),
                             0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                texture_sequence = shown->lease->sequence;
                texture_stream = shown->lease->stream_id;
                texture_low = color_low;
                texture_high = color_high;
            }
            if (ImPlot::BeginPlot("##range_doppler", {-1, -42}, ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Excess bistatic range (km)", "Doppler (Hz)");
                ImPlot::PlotImage("Power", ImTextureID(preview_texture),
                                  {0, number(d, "frequency_start")},
                                  {number(d, "range_end_km"), number(d, "frequency_end")});
                ImPlot::EndPlot();
            }
            ImPlot::PopColormap();
            if (!auto_scale)
                ImGui::DragFloatRange2("Colour dB", &color_low, &color_high, .25f, -200, 100);
        } else {
            ImGui::Spacing();
            ImGui::TextColored(muted, "No range–Doppler preview");
            ImGui::TextWrapped("Start a capture and select a channel. Scientific profiles are "
                               "independent of this display.");
        }
        ImGui::End();
        ImGui::Begin("Navigation");
        navigation_badge(nav);
        if (!command_errors["ins"].empty())
            ImGui::TextWrapped("%s", command_errors["ins"].c_str());
        label("Estimator", text(nav, "stage"));
        const auto age =
            nav.contains("state_monotonic_ns")
                ? (double(pl64::monotonic_ns()) - number(nav, "state_monotonic_ns")) * 1e-6
                : 0;
        if (age > 250)
            ImGui::TextColored(amber, "Navigation state is stale (%.0f ms)", age);
        if (ImGui::BeginTable("navigation_metrics", 2, ImGuiTableFlags_RowBg)) {
            metric("GNSS age", number(nav, "gnss_age_s"), "s", 2);
            metric("Filter update", number(nav, "update_ms"), "ms", 3);
            metric("Normalized residual", number(nav, "normalized_residual"));
            ImGui::EndTable();
        }
        if (nav.contains("values") && nav["values"].is_array() && nav["values"].size() >= 16) {
            const auto &v = nav["values"];
            if (ImGui::BeginTable("state_vector", 4, ImGuiTableFlags_RowBg)) {
                for (const char *title : {"NED", "North", "East", "Down"})
                    ImGui::TableSetupColumn(title);
                ImGui::TableHeadersRow();
                for (unsigned group = 0; group < 2; ++group) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(group ? "Velocity m/s" : "Position m");
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", v[group * 3 + axis].get<double>());
                    }
                }
                ImGui::EndTable();
            }
            if (ImGui::CollapsingHeader("Attitude & biases")) {
                for (unsigned i = 6; i < 16; ++i) {
                    ImGui::Text("%s %u",
                                i < 10   ? "Quaternion xyzw"
                                : i < 13 ? "Gyro bias rad/s"
                                         : "Accel bias m/s²",
                                i < 10   ? i - 6
                                : i < 13 ? i - 10
                                         : i - 13);
                    ImGui::SameLine(205);
                    ImGui::Text("%.6f", v[i].get<double>());
                }
            }
        }
        ImGui::BeginDisabled(!nav.value("connected", false));
        if (ImGui::Button("Reset alignment"))
            model.command("ins", {{"op", "reset"}});
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextWrapped("Readiness and timing uncertainty remain authoritative.");
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::Begin("Recording");
        state_badge(recorder);
        bool enabled = recorder.value("enabled", false);
        ImGui::BeginDisabled(!recorder.value("connected", false));
        if (ImGui::Checkbox("Record to disk", &enabled))
            model.command("recorder", {{"op", "recording"}, {"enabled", enabled}});
        for (const auto *source : {"ins", "radar", "sar"}) {
            bool selected = recorder.value("selected", Json::object()).value(source, true);
            const char *title = std::string_view(source) == "ins"     ? "Navigation"
                                : std::string_view(source) == "radar" ? "DVB-T profiles"
                                                                      : "SAR outputs";
            if (ImGui::Checkbox(title, &selected))
                model.command("recorder",
                              {{"op", "select"}, {"source", source}, {"enabled", selected}});
        }
        ImGui::EndDisabled();
        const auto recording_path = text(recorder, "file", "");
        ImGui::TextWrapped("%s", std::filesystem::path(recording_path).filename().c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", recording_path.c_str());
        ImGui::TextColored(muted, "%.1f MiB in current file",
                           number(recorder, "bytes", 0) / 1048576);
        if (!text(recorder, "error", "").empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, amber);
            ImGui::TextWrapped("%s", text(recorder, "error").c_str());
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::Begin("SAR / ISAR");
        state_badge(sar);
        if (!command_errors["sar"].empty())
            ImGui::TextWrapped("%s", command_errors["sar"].c_str());
        ImGui::TextWrapped("%s", text(sar, "message", "Waiting for SAR service").c_str());
        ImGui::BeginDisabled(!sar.value("connected", false));
        if (ImGui::Button("Start subscriptions"))
            model.command("sar", {{"op", "start"}});
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
            model.command("sar", {{"op", "stop"}});
        ImGui::EndDisabled();
        if (sar.contains("inputs"))
            for (const auto &[name, input] : sar["inputs"].items()) {
                ImGui::SeparatorText(name.c_str());
                ImGui::Text("%s · %.0f records",
                            input.value("connected", false) ? "Connected" : "Waiting",
                            number(input, "records"));
            }
        ImGui::TextColored(muted, "No image formation or autofocus is installed.");
        ImGui::End();
        if (show_events) {
            ImGui::Begin("Events", &show_events);
            for (const auto &entry : events) {
                ImGui::TextWrapped("%s", entry.c_str());
                ImGui::Separator();
            }
            ImGui::End();
        }
        if (show_details) {
            ImGui::Begin("Service details", &show_details);
            for (const auto &[name, value] : status)
                if (ImGui::CollapsingHeader(name.c_str()))
                    json_tree(value);
            ImGui::End();
        }
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(.04f, .05f, .06f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frame_number;
        if (frames && frame_number >= frames) {
            if (!image.empty())
                screenshot(image, window);
            glfwSetWindowShouldClose(window, true);
        }
        glfwSwapBuffers(window);
        if (w == 0 || h == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        else if (!image.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    worker.request_stop();
    worker.join();
    if (preview_texture)
        glDeleteTextures(1, &preview_texture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
