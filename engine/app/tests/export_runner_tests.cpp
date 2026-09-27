// The editor's reading of an export (ADR 0104 §4-5): the lines `ludwerk build
// --progress=json` writes, turned into steps and a result; the status line the
// target cards are drawn from; `adb devices -l`; and the recent list.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

#include "engine/app/export_runner.h"

using namespace engine;

TEST_CASE("the progress lines become steps, a result and a log")
{
    app::ExportRun run;
    // Split mid-line, as a pipe hands it over.
    run.feed("{\"step\":\"check\",\"state\":\"start\"}\n{\"step\":\"check\",\"sta");
    REQUIRE(run.steps().size() == 1);
    CHECK(run.steps()[0].state == app::ExportStep::State::Running);
    run.feed("te\":\"done\",\"ms\":12}\r\nassetc: 3 mesh(es)\n");
    CHECK(run.steps()[0].state == app::ExportStep::State::Done);
    CHECK(run.steps()[0].ms == doctest::Approx(12.0));
    CHECK(run.log().find("assetc: 3 mesh(es)") != std::string::npos);

    run.feed("{\"step\":\"pack\",\"state\":\"start\"}\n");
    run.feed("{\"step\":\"pack\",\"state\":\"fail\",\"ms\":3,\"message\":\"The pack could not be built.\"}\n");
    REQUIRE(run.failure() != nullptr);
    CHECK(run.failure()->name == "pack");
    CHECK(run.failure()->message == "The pack could not be built.");

    run.feed("{\"note\":\"Built it.\"}\n{\"result\":{\"target\":\"android\",\"apk\":\"dist/android/g-1.0.0.apk\","
             "\"package\":\"dev.g\"}}\n");
    REQUIRE(run.notes().size() == 1);
    REQUIRE(run.result().has_value());
    CHECK(run.result()->target == "android");
    CHECK(run.result()->apk == "dist/android/g-1.0.0.apk");
    CHECK(run.result()->package == "dev.g");
}

TEST_CASE("the status line is the last JSON line, and anything else is not one")
{
    const auto status = app::parseExportStatus(
        "lute: warming up\n{\"targets\":[{\"name\":\"windows\",\"server\":false,\"ready\":true,\"player\":true,"
        "\"tools\":true},{\"name\":\"android\",\"server\":false,\"ready\":false,\"player\":true,\"tools\":false}],"
        "\"adb\":\"C:/sdk/platform-tools/adb.exe\"}\r\n");
    REQUIRE(status.has_value());
    REQUIRE(status->targets.size() == 2);
    CHECK(status->targets[0].ready);
    CHECK_FALSE(status->targets[1].ready);
    CHECK_FALSE(status->targets[1].tools);
    CHECK(status->adb == "C:/sdk/platform-tools/adb.exe");
    CHECK_FALSE(app::parseExportStatus("no JSON here\n").has_value());
}

TEST_CASE("adb names the phone attached, and not one it may not talk to")
{
    CHECK(app::parseAdbDevice("List of devices attached\n"
                              "R5CX123 device product:e3qxxx model:SM_S938B device:e3q transport_id:1\n\n") ==
          "SM S938B");
    CHECK(app::parseAdbDevice("List of devices attached\nR5CX123 unauthorized usb:1-1 transport_id:2\n").empty());
    CHECK(app::parseAdbDevice("List of devices attached\n\n").empty());
}

TEST_CASE("the recent list keeps ten per project, newest first, and every project's own")
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "engine-recent-exports-test.json";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    const std::filesystem::path one = std::filesystem::temp_directory_path() / "projectone";
    const std::filesystem::path two = std::filesystem::temp_directory_path() / "projecttwo";

    for (int index = 0; index < 12; ++index)
        CHECK(app::rememberExport(file, one, {"windows", "1.0." + std::to_string(index), "now", 10, "dist/windows"}));
    CHECK(app::rememberExport(file, two, {"android", "2.0.0", "later", 20, "dist/android"}));

    const std::vector<app::RecentExport> first = app::loadRecentExports(file, one);
    REQUIRE(first.size() == 10);
    CHECK(first.front().version == "1.0.11");
    CHECK(first.back().version == "1.0.2");
    const std::vector<app::RecentExport> second = app::loadRecentExports(file, two);
    REQUIRE(second.size() == 1);
    CHECK(second.front().bytes == 20);
    std::filesystem::remove(file, ec);
}

TEST_CASE("the CLI is found beside an installed editor")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "engine-cli-locate-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "tools" / "cli");
    std::ofstream(root / "tools" / "cli" / "main.luau") << "-- the CLI\n";
#if defined(_WIN32)
    std::ofstream(root / "lute.exe") << "";
#else
    std::ofstream(root / "lute") << "";
#endif
    const std::optional<app::CliCommand> cli = app::locateCli(root, root / "somewhere");
    REQUIRE(cli.has_value());
    CHECK(cli->script == root / "tools" / "cli" / "main.luau");
    const std::vector<std::string> command = cli->command({"build", "--status"});
    REQUIRE(command.size() == 6);
    CHECK(command[1] == "run");
    CHECK(command[3] == "--");
    CHECK(command[5] == "--status");
    std::filesystem::remove_all(root, ec);
}
