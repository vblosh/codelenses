#include <filesystem>
#include <fstream>

#include "codelenses/app/config.hpp"
#include "codelenses/app/version.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("AppConfig defaults are sane and match requirements", "[config]") {
    auto config = codelenses::AppConfig::default_config();

    REQUIRE(config.server.host == "127.0.0.1");
    REQUIRE(config.server.port == 8080);
    REQUIRE(config.server.db_path == "codelenses.db");
    REQUIRE(config.server.log_level == "info");
    REQUIRE(config.server.worker_threads == 4);
    REQUIRE(config.server.enable_cors == true);
    REQUIRE_FALSE(config.server.initial_workspace.has_value());

    auto err = config.validate();
    REQUIRE_FALSE(err.has_value());
}

TEST_CASE("AppConfig validation catches erroneous values", "[config]") {
    SECTION("Empty host is invalid") {
        auto config = codelenses::AppConfig::default_config();
        config.server.host = "";
        auto err = config.validate();
        REQUIRE(err.has_value());
        if (err.has_value()) {
            REQUIRE(err.value().find("host") != std::string::npos);
        }
    }

    SECTION("Port 0 is invalid") {
        auto config = codelenses::AppConfig::default_config();
        config.server.port = 0;
        auto err = config.validate();
        REQUIRE(err.has_value());
        if (err.has_value()) {
            REQUIRE(err.value().find("port") != std::string::npos);
        }
    }

    SECTION("Empty db path is invalid") {
        auto config = codelenses::AppConfig::default_config();
        config.server.db_path = "";
        auto err = config.validate();
        REQUIRE(err.has_value());
        if (err.has_value()) {
            REQUIRE(err.value().find("Database") != std::string::npos);
        }
    }

    SECTION("Invalid log level is rejected") {
        auto config = codelenses::AppConfig::default_config();
        config.server.log_level = "verbose";
        auto err = config.validate();
        REQUIRE(err.has_value());
        if (err.has_value()) {
            REQUIRE(err.value().find("log level") != std::string::npos);
        }
    }

    SECTION("Zero worker threads is rejected") {
        auto config = codelenses::AppConfig::default_config();
        config.server.worker_threads = 0;
        auto err = config.validate();
        REQUIRE(err.has_value());
        if (err.has_value()) {
            REQUIRE(err.value().find("Worker threads") != std::string::npos);
        }
    }
}

TEST_CASE("AppConfig JSON serialization round-trips cleanly", "[config]") {
    auto config = codelenses::AppConfig::default_config();
    config.server.host = "0.0.0.0";
    config.server.port = 9000;
    config.server.db_path = "/tmp/test.db";
    config.server.initial_workspace = "/workspace/project";
    config.server.log_level = "debug";
    config.server.worker_threads = 8;
    config.server.enable_cors = false;
    config.server.static_dir = "/var/www";

    nlohmann::json json_out = config.to_json();
    auto config_in = codelenses::AppConfig::from_json(json_out);

    REQUIRE(config_in.server == config.server);
}

TEST_CASE("AppConfig JSON port validation prevents narrowing out-of-range values",
          "[config][json]") {
    SECTION("Port 65537 is rejected without narrowing") {
        nlohmann::json j = {{"server", {{"port", 65537}}}};
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json(j), std::out_of_range);
    }

    SECTION("Port 70000 is rejected") {
        nlohmann::json j = {{"server", {{"port", 70000}}}};
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json(j), std::out_of_range);
    }

    SECTION("Port 0 is rejected") {
        nlohmann::json j = {{"server", {{"port", 0}}}};
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json(j), std::out_of_range);
    }

    SECTION("Negative port is rejected") {
        nlohmann::json j = {{"server", {{"port", -1}}}};
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json(j), std::out_of_range);
    }

    SECTION("Zero worker threads in JSON is rejected") {
        nlohmann::json j = {{"server", {{"worker_threads", 0}}}};
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json(j), std::out_of_range);
    }

    SECTION("Negative worker threads in JSON is rejected") {
        nlohmann::json j = {{"server", {{"worker_threads", -2}}}};
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json(j), std::out_of_range);
    }
}

TEST_CASE("AppConfig loads from file", "[config]") {
    std::filesystem::path temp_config =
        std::filesystem::temp_directory_path() / "test_codelenses_config.json";

    nlohmann::json sample = {{"server",
                              {{"host", "127.0.0.2"},
                               {"port", 9191},
                               {"db_path", "custom.db"},
                               {"log_level", "trace"}}}};

    {
        std::ofstream out(temp_config);
        out << sample.dump();
    }

    auto config = codelenses::AppConfig::from_json_file(temp_config);
    std::filesystem::remove(temp_config);

    REQUIRE(config.server.host == "127.0.0.2");
    REQUIRE(config.server.port == 9191);
    REQUIRE(config.server.db_path == "custom.db");
    REQUIRE(config.server.log_level == "trace");

    SECTION("Non-existent config file throws") {
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json_file("/non/existent/path/config.json"),
                          std::runtime_error);
    }

    SECTION("Config file with out-of-range port throws") {
        std::filesystem::path bad_config =
            std::filesystem::temp_directory_path() / "test_codelenses_bad_port.json";
        nlohmann::json bad_sample = {{"server", {{"port", 65537}}}};
        {
            std::ofstream out(bad_config);
            out << bad_sample.dump();
        }
        REQUIRE_THROWS_AS(codelenses::AppConfig::from_json_file(bad_config), std::runtime_error);
        std::filesystem::remove(bad_config);
    }
}

TEST_CASE("CLI argument parser handles commands and flags", "[config][cli]") {
    SECTION("--help flag") {
        const char* argv[] = {"codelenses", "--help"};
        auto result = codelenses::AppConfig::parse_cli(2, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 0);
        REQUIRE(result.message.find("Usage:") != std::string::npos);
    }

    SECTION("-v version flag") {
        const char* argv[] = {"codelenses", "-v"};
        auto result = codelenses::AppConfig::parse_cli(2, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 0);
        REQUIRE(result.message.find(std::string(codelenses::kVersionString)) != std::string::npos);
    }

    SECTION("Custom options override defaults") {
        const char* argv[] = {"codelenses", "--host", "192.168.1.50",    "-p", "9999", "-d",
                              "test.db",    "-w",     "/home/user/repo", "-l", "warn", "-t",
                              "2"};
        auto result = codelenses::AppConfig::parse_cli(13, argv);
        REQUIRE_FALSE(result.should_exit);
        REQUIRE(result.config.server.host == "192.168.1.50");
        REQUIRE(result.config.server.port == 9999);
        REQUIRE(result.config.server.db_path == "test.db");
        REQUIRE(result.config.server.initial_workspace == "/home/user/repo");
        REQUIRE(result.config.server.log_level == "warn");
        REQUIRE(result.config.server.worker_threads == 2);
    }

    SECTION("Invalid port is rejected") {
        const char* argv[] = {"codelenses", "-p", "99999"};
        auto result = codelenses::AppConfig::parse_cli(3, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Invalid port") != std::string::npos);
    }

    SECTION("Malformed port with numeric prefix is rejected") {
        const char* argv[] = {"codelenses", "--port", "8080oops"};
        auto result = codelenses::AppConfig::parse_cli(3, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Invalid port number: 8080oops") != std::string::npos);
    }

    SECTION("Port with trailing suffix is rejected") {
        const char* argv[] = {"codelenses", "-p", "8080extra"};
        auto result = codelenses::AppConfig::parse_cli(3, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Invalid port number: 8080extra") != std::string::npos);
    }

    SECTION("Port zero is rejected") {
        const char* argv[] = {"codelenses", "-p", "0"};
        auto result = codelenses::AppConfig::parse_cli(3, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Invalid port number: 0") != std::string::npos);
    }

    SECTION("Negative port is rejected") {
        const char* argv[] = {"codelenses", "-p", "-80"};
        auto result = codelenses::AppConfig::parse_cli(3, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Invalid port number: -80") != std::string::npos);
    }

    SECTION("Malformed thread count with numeric prefix is rejected") {
        const char* argv[] = {"codelenses", "--threads", "4x"};
        auto result = codelenses::AppConfig::parse_cli(3, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Invalid thread count: 4x") != std::string::npos);
    }

    SECTION("Zero thread count is rejected") {
        const char* argv[] = {"codelenses", "-t", "0"};
        auto result = codelenses::AppConfig::parse_cli(3, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Invalid thread count: 0") != std::string::npos);
    }

    SECTION("Missing value for option is rejected") {
        const char* argv[] = {"codelenses", "--host"};
        auto result = codelenses::AppConfig::parse_cli(2, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("requires an address argument") != std::string::npos);
    }

    SECTION("Unknown option is rejected") {
        const char* argv[] = {"codelenses", "--unknown-flag"};
        auto result = codelenses::AppConfig::parse_cli(2, argv);
        REQUIRE(result.should_exit);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.message.find("Unknown argument") != std::string::npos);
    }
}
