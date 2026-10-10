// A stand-in for a user's native script module in the macOS export test: a real dylib that
// exports none of the user-module entry points, so the Player's load of it fails cleanly.
extern "C" int GE_MacBundleFixtureDylib();

extern "C" int GE_MacBundleFixtureDylib()
{
    return 0;
}
