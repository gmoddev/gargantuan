#include "gargantuan/classes/DataModel.hpp"

#include "gargantuan/services/ProcessService.hpp"
#include "gargantuan/services/RunService.hpp"
#include "gargantuan/services/Tags.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/Workspace.hpp"

namespace gargantuan {
	const DataModel::ServiceDefinitions &DataModel::GetServiceDefinitions() const {
		static const DataModel::ServiceDefinitions CONSTRUCTORS = {
			{"ProcessService", SchemaId::FromNativeName("Engine", "ProcessService")},
			{"RunService", SchemaId::FromNativeName("Engine", "RunService")},
			{"Tags", SchemaId::FromNativeName("Engine", "Tags")},
			{"UserInputService", SchemaId::FromNativeName("Engine", "UserInputService")},
			{"Workspace", SchemaId::FromNativeName("Engine", "Workspace")},
		};
		return CONSTRUCTORS;
	};
}
