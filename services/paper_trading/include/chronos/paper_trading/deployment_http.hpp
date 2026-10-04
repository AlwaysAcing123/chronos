#pragma once

#include "chronos/paper_trading/deployment_runtime.hpp"
#include "chronos/paper_trading/http_message.hpp"

#include <functional>
#include <string_view>
#include <variant>
#include <vector>

// The model-deployment routes of the paper-trading API (ADR 0018); see
// docs/paper-trading-api.md.
//
//   GET  /api/v1/deployments                      newest first; ?limit&cursor
//   POST /api/v1/deployments                      create (stopped)
//   POST /api/v1/deployments/kill-switch          stop every deployment
//   GET  /api/v1/deployments/{deployment_id}
//   POST /api/v1/deployments/{deployment_id}/start
//   POST /api/v1/deployments/{deployment_id}/stop
//   GET  /api/v1/deployments/{deployment_id}/decisions   newest first
//
// There is no route that submits a deployment's order: only the server's
// own runtime does, through PaperTradingService::submit_deployment_order.
// Nothing here authenticates anyone. These are local application rules for
// a single-user server on localhost, not access control.
namespace chronos::paper_trading {

// `parts` are the request path's segments, starting "api", "v1",
// "deployments".
[[nodiscard]] HttpResponse deployment_route(
    DeploymentManager& manager,
    const HttpRequest& request,
    const std::vector<std::string_view>& parts,
    const std::function<void(std::string_view)>& log
);

// The strict create request. Exposed for tests.
struct MalformedDeploymentRequest {
    std::string field;
    std::string message;
};

[[nodiscard]] std::variant<DeploymentCreateRequest, MalformedDeploymentRequest> parse_deployment_create_request(
    std::string_view body
);

}  // namespace chronos::paper_trading
