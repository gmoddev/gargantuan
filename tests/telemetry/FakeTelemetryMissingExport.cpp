#if defined(_WIN32)
#define TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

TEST_EXPORT int GargantuanTelemetry_NotTheApi() {
	return 0;
}
