#include "header_pair.hpp"

namespace App {

Service::Service(std::string name) : name_(std::move(name)), status_(ServiceStatus::Stopped) {}

Service::~Service() = default;

bool Service::start() {
    status_ = ServiceStatus::Running;
    return true;
}

void Service::stop() {
    status_ = ServiceStatus::Stopped;
}

ServiceStatus Service::status() const {
    return status_;
}

} // namespace App

void run_application() {
    App::Service s("primary_svc");
    s.start();
    if (s.status() == App::ServiceStatus::Running) {
        s.stop();
    }
}
