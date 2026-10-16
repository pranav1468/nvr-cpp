#include "nvr/dispatcher/command_dispatcher.h"
#include "nvr/dispatcher/rbac_validator.h"
#include "nvr/common/logger.h"

#include <cassert>
#include <iostream>
#include <chrono>

int main() {
    std::cout << "=== Running CommandDispatcher & RBAC Validation Suite ===" << std::endl;

    auto& rbac = nvr::RbacValidator::Instance();
    auto& dispatcher = nvr::CommandDispatcher::Instance();

    // 1. Test Authentication with valid credentials
    std::string admin_token;
    nvr::UserRole admin_role = nvr::UserRole::NONE;
    bool auth_ok = rbac.Authenticate("admin", "Admin@123", admin_token, admin_role);
    assert(auth_ok);
    assert(admin_role == nvr::UserRole::ADMIN);
    assert(!admin_token.empty());
    std::cout << "[PASS] Admin authentication successful, token generated (len=" << admin_token.size() << ")" << std::endl;

    // 2. Test Authentication with invalid credentials
    std::string bad_token;
    nvr::UserRole bad_role = nvr::UserRole::NONE;
    bool bad_auth = rbac.Authenticate("admin", "WrongPassword", bad_token, bad_role);
    assert(!bad_auth);
    std::cout << "[PASS] Bad credentials correctly rejected" << std::endl;

    // 3. Test Viewer Authentication
    std::string viewer_token;
    nvr::UserRole viewer_role = nvr::UserRole::NONE;
    bool viewer_ok = rbac.Authenticate("viewer", "Viewer@123", viewer_token, viewer_role);
    assert(viewer_ok);
    assert(viewer_role == nvr::UserRole::VIEWER);
    std::cout << "[PASS] Viewer authentication successful" << std::endl;

    // 4. Test Dispatch with missing/empty token on protected action
    nvr::CommandEnvelope unauth_cmd;
    unauth_cmd.action = "system.status";
    unauth_cmd.auth_token = ""; // empty
    auto unauth_res = dispatcher.Dispatch(unauth_cmd);
    assert(!unauth_res.success);
    assert(unauth_res.status_code == 401);
    std::cout << "[PASS] Unauthenticated request correctly rejected with 401 Unauthorized" << std::endl;

    // 5. Test Dispatch with Viewer token on Viewer action (system.status)
    nvr::CommandEnvelope viewer_cmd;
    viewer_cmd.action = "system.status";
    viewer_cmd.auth_token = viewer_token;
    auto viewer_res = dispatcher.Dispatch(viewer_cmd);
    assert(viewer_res.success);
    assert(viewer_res.status_code == 200);
    assert(viewer_res.data.contains("status"));
    assert(viewer_res.data["status"] == "ONLINE");
    std::cout << "[PASS] Viewer token successfully executed 'system.status' (status=ONLINE)" << std::endl;

    // 6. Test Privilege Escalation Prevention: Viewer attempting Operator action (layout.set)
    nvr::CommandEnvelope attack_cmd;
    attack_cmd.action = "layout.set";
    attack_cmd.auth_token = viewer_token; // Viewer trying to modify layout
    attack_cmd.payload["layout"] = 4;
    auto attack_res = dispatcher.Dispatch(attack_cmd);
    assert(!attack_res.success);
    assert(attack_res.status_code == 403);
    std::cout << "[PASS] Privilege escalation prevented: Viewer rejected from 'layout.set' with 403 Forbidden" << std::endl;

    // 7. Test Admin executing Operator action (layout.set)
    nvr::CommandEnvelope admin_cmd;
    admin_cmd.action = "layout.set";
    admin_cmd.auth_token = admin_token;
    admin_cmd.payload["layout"] = 4;
    auto admin_res = dispatcher.Dispatch(admin_cmd);
    assert(admin_res.success);
    assert(admin_res.status_code == 200);
    std::cout << "[PASS] Admin successfully executed 'layout.set'" << std::endl;

    // 8. Test JSON Dispatch API (as would be sent by WebSocket or REST API)
    std::string json_req = R"({
        "action": "layout.get",
        "auth_token": ")" + viewer_token + R"("
    })";
    auto json_res = dispatcher.DispatchJson(json_req);
    assert(json_res.success);
    assert(json_res.status_code == 200);
    assert(json_res.data.contains("layout"));
    assert(json_res.data["layout"] == 4);
    std::cout << "[PASS] DispatchJson executed successfully: layout=" << json_res.data["layout"] << std::endl;

    // 9. Test Malformed JSON Handling
    auto malformed_res = dispatcher.DispatchJson("{ not valid json !!! }");
    assert(!malformed_res.success);
    assert(malformed_res.status_code == 400);
    std::cout << "[PASS] Malformed JSON safely returned 400 Bad Request without crash" << std::endl;

    // 10. Test Token Revocation (Logout)
    rbac.RevokeSession(viewer_token);
    auto revoked_res = dispatcher.Dispatch(viewer_cmd);
    assert(!revoked_res.success);
    assert(revoked_res.status_code == 401);
    std::cout << "[PASS] Revoked session token immediately rejected with 401" << std::endl;

    // 11. Performance benchmark: Pure Dispatch routing latency (excluding console I/O)
    dispatcher.RegisterHandler("benchmark.ping", nvr::UserRole::VIEWER, [](const nvr::CommandEnvelope&) -> nvr::CommandResult {
        return nvr::CommandResult::Success();
    });

    nvr::CommandEnvelope ping_cmd;
    ping_cmd.action = "benchmark.ping";
    ping_cmd.auth_token = admin_token;

    constexpr int kBenchmarkIterations = 100000;
    auto start_time = std::chrono::steady_clock::now();
    for (int i = 0; i < kBenchmarkIterations; ++i) {
        dispatcher.Dispatch(ping_cmd);
    }
    auto end_time = std::chrono::steady_clock::now();
    double total_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    double latency_per_dispatch_us = total_us / kBenchmarkIterations;
    std::cout << "[PASS] Performance Benchmark: " << kBenchmarkIterations << " dispatches in " 
              << (total_us / 1000.0) << " ms -> " << latency_per_dispatch_us << " us / dispatch (limit: <10 us)" << std::endl;
    assert(latency_per_dispatch_us < 10.0);

    std::cout << "\n>>> ALL COMMAND DISPATCHER & RBAC VALIDATION TESTS PASSED DETERMINISTICALLY! <<<" << std::endl;
    return 0;
}
