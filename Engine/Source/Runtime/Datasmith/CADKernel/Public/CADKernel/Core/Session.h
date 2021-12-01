// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CADKernel/Core/Types.h"
#include "CADKernel/Core/Entity.h"
#include "CADKernel/Core/Database.h"

class FArchive;

namespace CADKernel
{
	class FEntity;
	class CADKERNEL_API FSession
	{
		friend FEntity;
		friend FCADKernelArchive;

	protected:

		double GeometricTolerance;
		FDatabase Database;
		int32 LastHostId;

	public:
#if defined(CADKERNEL_DEV) || defined(CADKERNEL_STDA)
		static FSession Session;
#endif

		FSession(double InGeometricTolerance)
			: GeometricTolerance(InGeometricTolerance)
		{
		}

		TSharedRef<FModel> GetModel();

		void Serialize(FCADKernelArchive& Ar)
		{
			Ar << GeometricTolerance;
		}

		FDatabase& GetDatabase()
		{
			return Database;
		}

		/**
		 * Tolerance must not be modified as soon as a geometric entity has been build.
		 */
		void SetGeometricTolerance(double NewTolerance);

		double GetGeometricTolerance() const
		{
			return GeometricTolerance;
		}

		/**
		 * Save the database as a FAchive in a file
		 * Mandatory: all entity have to have a defined ID
		 * Use SpawnEntityIdent if needed
		 */
		void SaveDatabase(const TCHAR* FilePath);

		/**
		 * Save a selection and all the dependencies as a FAchive in a file
		 * Mandatory: all entity have to have a defined ID
		 * Use SpawnEntityIdent if needed
		 */
		void SaveDatabase(const TCHAR* FileName, const TArray<TSharedPtr<FEntity>>& Entities);

		/**
		 * Save a selection and all the dependencies as a FAchive in a file
		 * Mandatory: all entity have to have a defined ID
		 * Use SpawnEntityIdent if needed
		 */
		void SaveDatabase(const TCHAR* FileName, const TSharedPtr<FEntity> Entity)
		{
			TArray<TSharedPtr<FEntity>> Entities;
			Entities.Emplace(Entity);
			SaveDatabase(FileName, Entities);
		}

		/**
		 * Load and add a database in the current session database
		 * Entity ID is set for all loaded entities
		 */
		void LoadDatabase(const TCHAR* FilePath);

		/**
		 * Add a database defined by a RawData in the current session database
		 * Entity ID is set for all loaded entities
		 */
		void AddDatabase(const TArray<uint8>& InRawData);

		void Clear()
		{
			Database.Empty();
		}

		/**
		 * To be consistent,  all entity to save have to had an Id.
		 * This method browses all sub entities and set their Id if needed
		 * @param bForceSpawning If false, the process does not iterate through the children of entities with a defined ID
		 */
		uint32 SpawnEntityIdent(const TArray<TSharedPtr<FEntity>>& SelectedEntities, bool bForceSpawning = false)
		{
			return Database.SpawnEntityIdent(SelectedEntities, bForceSpawning);
		}

		uint32 SpawnEntityIdent(TSharedPtr<FEntity> SelectedEntity, bool bForceSpawning)
		{
			return Database.SpawnEntityIdent(SelectedEntity, bForceSpawning);
		}

		template<typename PointType>
		uint32 SpawnEntityIdent(TSharedPtr<PointType>& SelectedEntity, bool bForceSpawning = false)
		{
			return Database.SpawnEntityIdent((TSharedPtr<FEntity>&) SelectedEntity, bForceSpawning);
		}

		int32 GetLastHostId() const 
		{
			return LastHostId;
		}

		int32 NewHostId()
		{
			return ++LastHostId;
		}

		/**
		 * For stitching purpose, stitching can generate new body needing a host id. 
		 * To avoid duplicate, the first generated host id can be set.
		 */
		void SetFirstNewHostId(int32 StartHostId)
		{
			LastHostId = StartHostId;
		}
	};

} // namespace CADKernel

