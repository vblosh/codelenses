#pragma once

#include <exception>
#include <optional>
#include <random>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace codelenses::server {

[[nodiscard]] inline std::string generate_request_id() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    uint64_t r1 = rng();
    uint64_t r2 = rng();
    std::ostringstream oss;
    oss << "req_" << std::hex << r1 << r2;
    return oss.str();
}

struct ApiError : public std::exception {
    int status_code{500};
    std::string code{"internal_error"};
    std::string message{"An internal error occurred."};
    std::optional<nlohmann::json> details{std::nullopt};

    ApiError(int status, std::string code_str, std::string message_str,
             std::optional<nlohmann::json> details_val = std::nullopt)
        : status_code(status), code(std::move(code_str)), message(std::move(message_str)),
          details(std::move(details_val)) {}

    [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }

    [[nodiscard]] static ApiError
    bad_request(std::string code, std::string message,
                std::optional<nlohmann::json> details = std::nullopt) {
        return ApiError(400, std::move(code), std::move(message), std::move(details));
    }

    [[nodiscard]] static ApiError forbidden(std::string code, std::string message) {
        return ApiError(403, std::move(code), std::move(message));
    }

    [[nodiscard]] static ApiError not_found(std::string code, std::string message) {
        return ApiError(404, std::move(code), std::move(message));
    }

    [[nodiscard]] static ApiError conflict(std::string code, std::string message) {
        return ApiError(409, std::move(code), std::move(message));
    }

    [[nodiscard]] static ApiError
    unprocessable(std::string code, std::string message,
                  std::optional<nlohmann::json> details = std::nullopt) {
        return ApiError(422, std::move(code), std::move(message), std::move(details));
    }

    [[nodiscard]] static ApiError internal_error(std::string message) {
        return ApiError(500, "internal_error", std::move(message));
    }
};

[[nodiscard]] inline nlohmann::json make_error_envelope(const ApiError& err,
                                                        const std::string& request_id) {
    nlohmann::json inner = {
        {"code", err.code},
        {"message", err.message},
        {"requestId", request_id},
    };
    if (err.details.has_value()) {
        inner["details"] = *err.details;
    } else {
        inner["details"] = nullptr;
    }

    nlohmann::json outer = inner;
    outer["error"] = inner;
    return outer;
}

} // namespace codelenses::server
