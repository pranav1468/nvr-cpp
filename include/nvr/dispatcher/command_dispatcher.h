#pragma once

#include "nvr/dispatcher/commands.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <shared_mutex>

namespace nvr {

using CommandHandler = std::function<CommandResult(const CommandEnvelope&)>;

struct RegisteredCommand {
    std::string action;
    UserRole min_role{UserRole::VIEWER};
    CommandHandler handler;
    std::string description;
};

class CommandDispatcher {
public:
    static CommandDispatcher& Instance();

    // Register an action handler with minimum required role
    void RegisterHandler(const std::string& action, UserRole min_role, 
                         CommandHandler handler, const std::string& description = "");

    // Unregister action
    void UnregisterHandler(const std::string& action);

    // Dispatch a structured command envelope
    CommandResult Dispatch(const CommandEnvelope& cmd);

    // Dispatch from raw JSON string (e.g. from WebSocket or REST body)
    CommandResult DispatchJson(const std::string& json_str, const std::string& auth_token_override = "");

    // Register all default baseline subsystem handlers
    void RegisterDefaultHandlers();

    // Query list of registered commands (for API documentation or discovery)
    nlohmann::json GetRegisteredCommands() const;

private:
    CommandDispatcher();
    ~CommandDispatcher() = default;
    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, RegisteredCommand> handlers_;
};

} // namespace nvr
