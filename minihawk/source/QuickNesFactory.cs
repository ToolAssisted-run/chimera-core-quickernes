using System.Collections.Generic;

using System.Linq;

using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores.Nintendo.NES;

namespace BizHawk.Emulation.Cores.Consoles.Nintendo.QuickNES
{
	/// <summary>miniHawk core factory for QuickNES (quickerNES).</summary>
	public sealed class QuickNesFactory : ICoreFactory
	{
		public string CoreName => "QuickNes";

		public IReadOnlyList<string> SystemIds { get; } = [ VSystemID.Raw.NES ];

		public Type CoreType => typeof(QuickNES);

		public Type SettingsType => typeof(QuickNES.QuickNESSettings);

		public Type SyncSettingsType => typeof(QuickNES.QuickNESSyncSettings);

		public IEmulator Create(CoreCreationContext ctx)
		{
			var rom = ctx.Roms.FirstOrDefault()
				?? throw new InvalidOperationException($"{CoreName} needs a rom to load");
			return new QuickNES(
				rom.FileData,
				(QuickNES.QuickNESSettings) ctx.Settings,
				(QuickNES.QuickNESSyncSettings) ctx.SyncSettings);
		}
	}
}
