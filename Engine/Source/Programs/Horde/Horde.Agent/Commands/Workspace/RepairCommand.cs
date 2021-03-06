// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Perforce.Managed;
using Microsoft.Extensions.Logging;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace HordeAgent.Commands.Workspace
{
	[Command("Workspace", "RepairCache", "Checks the integrity of the cache, and removes any invalid files")]
	class RepairCommand : WorkspaceCommand
	{
		protected override Task ExecuteAsync(ManagedWorkspace Repo, ILogger Logger)
		{
			return Repo.RepairAsync(CancellationToken.None);
		}
	}
}
