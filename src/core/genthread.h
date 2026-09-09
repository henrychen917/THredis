// Boot-selected fused runtime. Kept out of main.cc so its template instantiations cannot perturb
// the split loop translation unit's optimizer decisions.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace tomo {

class AofReplayPlan;
class LateUnixListener;
class Server;
class ShutdownReportFinalLine;
class SnapshotLoadPlan;
class TlsContext;

int run_fused_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
                     LateUnixListener& unix_listener,
                     ShutdownReportFinalLine& final_report);

// Split placement with shard-less fused-capable IO and the existing owner role loop.
int run_split_read_local_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
                     LateUnixListener& unix_listener,
                     ShutdownReportFinalLine& final_report);

// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
void append_read_local_thread_info(std::string& body, const Server& server);

}  // namespace tomo
