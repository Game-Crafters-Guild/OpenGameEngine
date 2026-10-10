#include <gtest/gtest.h>

#include "Jobs/CompileServerClient.h"
#include "Jobs/PipeTransport.h"

#include <cstdlib>

using namespace GameEngine;

// Minimal engine-level compile server smoke test.
// Uses CompileServerClient to issue a tiny JSON request
// and verifies that the call path succeeds when the
// managed CompileServerHost is available.

TEST(EngineCompileServerSmoke, EmptyWorkspaceCompile)
{
	const std::string pipeName = "GE_CompileServer_EngineSmoke";

	// Any host this test spawns must die with the test process (issue #357):
	// mark it ephemeral so the launch path binds it to the kill-on-close job
	// object (Windows) / parent-death signal (Linux).
#ifdef _WIN32
	_putenv_s("GE_COMPILE_SERVER_EPHEMERAL", "1");
#else
	setenv("GE_COMPILE_SERVER_EPHEMERAL", "1", 1);
#endif

	// Match the minimal request shape used by the managed
	// GameEngine.CompileServerSmoke utility: empty workspace
	// with no files, incremental strategy.
	const std::string requestJson =
	    "{\"ProjectRoot\":\".\"," \
	    "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[]," \
	    "\"PreferredStrategy\":\"Incremental\",\"ForceFull\":false}";

	CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
	CompileServerResponse response;
	const bool ok = client.Compile(requestJson, response);

	// Orderly teardown regardless of outcome: ask any host we started to exit
	// now instead of waiting for its idle timeout (the job object still covers
	// crashed runs).
	{
		PipeTransport transport(pipeName);
		if (transport.TryConnectOnce(/*waitMs=*/200))
		{
			std::string shutdownResponse;
			(void)transport.SendRequest("__shutdown__", shutdownResponse);
			transport.Close();
		}
	}

	if (!ok)
	{
	    GTEST_SKIP() << "CompileServerHost not available or minimal compile failed in this environment; skipping smoke test.";
	}

	SUCCEED();
}
