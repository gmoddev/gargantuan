# Windows anonymous-pipe reads can occupy a ThreadPool worker while idle.
# The 32-client farm owns 66 redirected streams, so give each a dedicated
# bounded-lifetime drain instead of starving scheduling and evidence polling.
if (-not ('PhysicalFarmPipeDrain' -as [type])) {
	Add-Type -TypeDefinition @'
using System.IO;
using System.Threading;
using System.Threading.Tasks;

public static class PhysicalFarmPipeDrain {
	public static Task Start(Stream Source, Stream Destination) {
		return Task.Factory.StartNew(() => Source.CopyTo(Destination),
			CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
	}
}
'@
}
