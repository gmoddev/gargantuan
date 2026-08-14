#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

int main() {
	using namespace gargantuan;
	const auto root = std::filesystem::temp_directory_path() /
		("gargantuan-prerun-bootstrap-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()
		));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() { std::filesystem::remove_all(Root); }
	} cleanup{root};
	std::filesystem::create_directories(root / ".gargantuan");
	std::ofstream source(root / ".gargantuan" / "prerun.luau", std::ios::binary);
	source << R"(Schema:RegisterEnum({
		Namespace = "Game",
		Name = "BootstrapState",
		Version = 1,
		Items = { Ready = 1 },
	}))";
	source.close();

	try {
		BootstrapProjectRuntimeSchema(root);
		const auto &lifecycle = GetRuntimeSchemaLifecycle();
		if (lifecycle.GetActiveGeneration() != 1 ||
			lifecycle.GetActiveRegistry()->FindEnumByName("Game.BootstrapState") == nullptr) {
			std::cerr << "Project startup did not publish one complete generation\n";
			return 1;
		}
		auto world = std::make_shared<DataModel>();
		if (!world) return 1;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	std::cout << "PreRun bootstrap ordering passed\n";
	return 0;
}
