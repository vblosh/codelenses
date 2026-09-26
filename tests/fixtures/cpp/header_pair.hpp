#pragma once

#include <string>

namespace App {

enum class ServiceStatus { Starting, Running, Stopped };

class BaseService {
public:
    virtual ~BaseService() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
};

class Service : public BaseService {
public:
    explicit Service(std::string name);
    ~Service() override;

    bool start() override;
    void stop() override;
    ServiceStatus status() const;

private:
    std::string name_;
    ServiceStatus status_;
};

} // namespace App
