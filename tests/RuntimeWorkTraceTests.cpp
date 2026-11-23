#include "runtime/RuntimeWorkTrace.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
	using namespace gargantuan::runtime_detail;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	struct TraceOutput {
		std::filesystem::path Path = std::filesystem::temp_directory_path() /
			("gargantuan-work-trace-" + std::to_string(WorkTimestamp()) + ".json");
		bool Created = false;

		~TraceOutput() {
			std::error_code Error;
			if (Created) std::filesystem::remove(Path, Error);
		}

		std::string Read() const {
			std::ifstream Input(Path, std::ios::binary);
			Check(static_cast<bool>(Input), "trace output can be opened");
			return {std::istreambuf_iterator<char>(Input), {}};
		}
	};

	void TestStructuredCollection() {
		TraceOutput Output;
		WorkTrace::Options Settings{.Path = Output.Path, .Capacity = 16, .Stride = 2,
			.FirstTick = 8, .LastTick = 10, .Detailed = true};
		Settings.Phases.reset();
		Settings.Phases.set(static_cast<std::size_t>(WorkPhase::Physics));
		WorkTrace Trace(std::move(Settings));
		Check(Trace.SelectFrame(2, 8) && Trace.SelectFrame(4, 10), "stride accepts within the inclusive tick range");
		Check(!Trace.SelectFrame(2, 7) && !Trace.SelectFrame(3, 9) && !Trace.SelectFrame(4, 11),
			"stride and the tick range both filter selection");
		WorkSample Sample{};
		{
			WorkTrace::Region Region(&Trace, 8, 0);
			WorkTrace::Region::SetCurrentLane(3);
			WorkCapture Capture(&Sample);
			WorkScope Step(WorkPhase::EngineStep);
			WorkScope Physics(WorkPhase::Physics);
			RecordWorkUnits(WorkPhase::Physics, 5);
			CountWork(WorkCounter::ClientReceiverVisits, 6);
			auto *Producer = CurrentProducerSample(WorkProducer::Structural);
			Producer->Enqueued = 1;
			Producer->QueueDepthMaximum = 7;
		}
		Check(!ActiveWorkSample && !ActiveWorkSpanSink, "a trace region restores thread-local capture state");
		std::thread Worker([&] {
			WorkSample WorkerSample{};
			WorkTrace::Region Region(&Trace, 8, 4);
			WorkCapture Capture(&WorkerSample);
			WorkScope Physics(WorkPhase::Physics);
		});
		Worker.join();
		Trace.RecordFrame(8, 0, WorkTimestamp(), 10, 2, true);
		Trace.RecordSample(8, 0, WorkTimestamp(), Sample);
		Check(Trace.Dropped() == 0, "trace retains all test records");
		Trace.Write();
		Output.Created = true;
		const auto Text = Output.Read();
		Check(Text.contains("\"cat\":\"scope\"") && Text.contains("\"cat\":\"phase\""), "span and phase records export");
		Check(Text.contains("\"tid\":1") && Text.contains("\"tid\":2"), "separate threads receive stable tracks");
		Check(Text.contains("\"tick\":8,\"lane\":3") && Text.contains("\"tick\":8,\"lane\":4"),
			"the logical lane correlates scope records independently of thread ID");
		Check(Text.contains("\"units\":5") && Text.contains("\"maximum_depth\":7") && Text.contains("\"value\":6"),
			"phase units, queue pressure and counters export");
		Check(Text.contains("\"deadline_missed\":true") && !Text.contains("EngineStep"),
			"deadline data exports and the phase mask applies");
		bool RefusedOverwrite = false;
		try { Trace.Write(); }
		catch (const std::runtime_error &) { RefusedOverwrite = true; }
		Check(RefusedOverwrite && Output.Read() == Text, "a trace cannot overwrite an existing file");
	}

	void TestBoundedSummary() {
		TraceOutput Output;
		WorkTrace Trace({.Path = Output.Path, .Capacity = 1});
		WorkSample Sample{};
		{
			WorkTrace::Region Region(&Trace, 1, 0);
			WorkCapture Capture(&Sample);
			WorkScope Physics(WorkPhase::Physics);
		}
		Trace.RecordSample(1, 0, WorkTimestamp(), Sample);
		Trace.RecordFrame(1, 0, WorkTimestamp(), 10);
		Check(Trace.Dropped() == 1, "fixed capacity rejects later records");
		Trace.Write();
		Output.Created = true;
		const auto Text = Output.Read();
		Check(Text.contains("\"mode\":\"summary\"") && Text.contains("\"cat\":\"phase\"") && !Text.contains("\"cat\":\"scope\""),
			"summary captures aggregate data");
		Check(Text.contains("\"records\":1,\"dropped\":1,\"capacity\":1"), "overflows are reported exactly");
	}
}

int main() {
	try {
		TestStructuredCollection();
		TestBoundedSummary();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Runtime:WorkTrace] " << Error.what() << '\n';
		return 1;
	}
}
