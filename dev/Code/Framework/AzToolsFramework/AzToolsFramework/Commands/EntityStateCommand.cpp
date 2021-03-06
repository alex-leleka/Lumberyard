/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#include "stdafx.h"
#include "EntityStateCommand.h"
#include <AzCore/Component/Entity.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Debug/Profiler.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Serialization/objectstream.h>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/IO/genericstreams.h>
#include <AzCore/IO/ByteContainerStream.h>
#include <AzCore/Slice/SliceComponent.h>
#include <AzCore/Asset/AssetManager.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include "PreemptiveUndoCache.h"

#include <AzToolsFramework/ToolsComponents/TransformComponent.h>

// until we have some growing memory array:
#define STATIC_BUFFERSIZE (1024 * 64)

namespace AzToolsFramework
{
    namespace
    {
        void LoadEntity(void* pClassPtr, const AZ::Uuid& uuidValue, const AZ::SerializeContext* sc, AZ::Entity** destPtr)
        {
            // entity is loaded.
            *destPtr = sc->Cast<AZ::Entity*>(pClassPtr, uuidValue);
            AZ_Assert(*destPtr, "Could not cast object to entity!");
        }
    }

    EntityStateCommand::EntityStateCommand(UndoSystem::URCommandID ID, const char* friendlyName)
        : UndoSystem::URSequencePoint(friendlyName ? friendlyName : "Entity Change", ID)
    {
        m_entityID = AZ::EntityId(0);
        m_entityContextId = AzFramework::EntityContextId::CreateNull();
        m_entityState = 0;
        m_isSelected = false;
    }
    EntityStateCommand::~EntityStateCommand()
    {
    }

    void EntityStateCommand::Capture(AZ::Entity* pSourceEntity, bool captureUndo)
    {
        AZ_PROFILE_FUNCTION(AZ::Debug::ProfileCategory::AzToolsFramework);

        m_entityID = pSourceEntity->GetId();
        EBUS_EVENT_ID_RESULT(m_entityContextId, m_entityID, AzFramework::EntityIdContextQueryBus, GetOwningContextId);
        EBUS_EVENT_RESULT(m_isSelected, AzToolsFramework::ToolsApplicationRequests::Bus, IsSelected, m_entityID);

        m_sliceRestoreInfo = AZ::SliceComponent::EntityRestoreInfo();

        AZ_Assert(pSourceEntity, "Null entity for undo");
        AZ_Assert(PreemptiveUndoCache::Get(), "You need a pre-emptive undo cache instance to exist for this to work.");
        AZ_Assert((!captureUndo) || (m_undoState.empty()), "You can't capture undo more than once");
        AZ::SerializeContext* sc = nullptr;
        EBUS_EVENT_RESULT(sc, AZ::ComponentApplicationBus, GetSerializeContext);
        AZ_Assert(sc, "Serialization context not found!");

        m_entityState = pSourceEntity->GetState();

        // The entity is loose, so we capture it directly.
        if (captureUndo)
        {
            m_undoState = PreemptiveUndoCache::Get()->Retrieve(m_entityID);
            if (m_undoState.empty())
            {
                PreemptiveUndoCache::Get()->UpdateCache(m_entityID);
                m_undoState = PreemptiveUndoCache::Get()->Retrieve(m_entityID);
            }
            AZ_Assert(!m_undoState.empty(), "Invalid empty size for the undo state of an entity.");
        }
        else
        {
            m_redoState.clear();
            AZ::IO::ByteContainerStream<AZStd::vector<AZ::u8> > ms(&m_redoState);
            AZ::ObjectStream* objStream = AZ::ObjectStream::Create(&ms, *sc, AZ::ObjectStream::ST_BINARY);
            if (!objStream->WriteClass(pSourceEntity))
            {
                AZ_Assert(false, "Unable to serialize entity for undo/redo. ObjectStream::WriteClass() returned an error.");
            }
            if (!objStream->Finalize())
            {
                AZ_Assert(false, "Unable to serialize entity for undo/redo. ObjectStream::Finalize() returned an error.");
            }
        }

        // If slice-owned, extract the data we need to restore it.
        AZ::SliceComponent::SliceInstanceAddress sliceInstanceAddr;
        EBUS_EVENT_ID_RESULT(sliceInstanceAddr, m_entityID, AzFramework::EntityIdContextQueryBus, GetOwningSlice);
        if (sliceInstanceAddr.first)
        {
            AZ::SliceComponent* rootSlice = nullptr;
            EBUS_EVENT_RESULT(rootSlice, EditorEntityContextRequestBus, GetEditorRootSlice);
            AZ_Assert(rootSlice, "Failed to retrieve editor root slice.");
            rootSlice->GetEntityRestoreInfo(m_entityID, m_sliceRestoreInfo);
        }
    }

    void EntityStateCommand::RestoreEntity(const AZ::u8* buffer, AZStd::size_t bufferSizeBytes) const
    {
        AZ_PROFILE_FUNCTION(AZ::Debug::ProfileCategory::AzToolsFramework);

        AZ_Assert(buffer, "No data to undo!");
        AZ_Assert(bufferSizeBytes, "Undo data is empty.");

        AZ::SerializeContext* serializeContext = nullptr;
        EBUS_EVENT_RESULT(serializeContext, AZ::ComponentApplicationBus, GetSerializeContext);
        AZ_Assert(serializeContext, "Serialization context not found!");
        AZ::IO::MemoryStream memoryStream(reinterpret_cast<const char*>(buffer), bufferSizeBytes);

        // If restoring to a slice, keep a reference to the slice asset so it isn't released when the entity
        // is deleted, only to immediately reload upon restoring.
        AZ::Data::Asset<AZ::SliceAsset> asset;
        if (m_sliceRestoreInfo)
        {
            asset = AZ::Data::AssetManager::Instance().FindAsset(m_sliceRestoreInfo.m_assetId);
        }

        // We have to delete the entity. If it's currently selected, make sure we re-select after re-creating.
        AzToolsFramework::EntityIdList selectedEntities;
        EBUS_EVENT_RESULT(selectedEntities, ToolsApplicationRequests::Bus, GetSelectedEntities);

        AZ::Entity* entity = nullptr;

        EBUS_EVENT_RESULT(entity, AZ::ComponentApplicationBus, FindEntity, m_entityID);

        EBUS_EVENT(AZ::ComponentApplicationBus, DeleteEntity, m_entityID);
        bool success = AZ::ObjectStream::LoadBlocking(&memoryStream, *serializeContext,
                AZStd::bind(&LoadEntity, AZStd::placeholders::_1, AZStd::placeholders::_2, AZStd::placeholders::_3, &entity),
                AZ::ObjectStream::FilterDescriptor(AZ::ObjectStream::AssetFilterNoAssetLoading));
        (void)success;
        AZ_Assert(success, "Unable to serialize entity for undo/redo");
        AZ_Assert(entity, "Unable to create entity");

        if (entity)
        {
            if (m_sliceRestoreInfo)
            {
                EBUS_EVENT(AzToolsFramework::EditorEntityContextRequestBus, RestoreSliceEntity, entity, m_sliceRestoreInfo);
            }
            else
            {
                if (!m_entityContextId.IsNull())
                {
                    EBUS_EVENT_ID(m_entityContextId, AzFramework::EntityContextRequestBus, AddEntity, entity);
                }

                if (m_entityState == AZ::Entity::ES_INIT || m_entityState == AZ::Entity::ES_ACTIVE)
                {
                    if (entity->GetState() == AZ::Entity::ES_CONSTRUCTED)
                    {
                        entity->Init();
                    }
                }
                if (m_entityState == AZ::Entity::ES_ACTIVE)
                {
                    if (entity->GetState() == AZ::Entity::ES_INIT)
                    {
                        entity->Activate();
                    }
                }
            }

            PreemptiveUndoCache::Get()->UpdateCache(m_entityID);

            if (m_isSelected)
            {
                selectedEntities.push_back(m_entityID);
            }
        }

        EBUS_EVENT(ToolsApplicationRequests::Bus, SetSelectedEntities, selectedEntities);
    }

    void EntityStateCommand::Undo()
    {
        RestoreEntity(m_undoState.data(), m_undoState.size());
    }

    void EntityStateCommand::Redo()
    {
        RestoreEntity(m_redoState.data(), m_redoState.size());
    }

    EntityDeleteCommand::EntityDeleteCommand(UndoSystem::URCommandID ID)
        : EntityStateCommand(ID, "Delete Entity")
    {
    }

    void EntityDeleteCommand::Capture(AZ::Entity* pSourceEntity)
    {
        PreemptiveUndoCache::Get()->UpdateCache(pSourceEntity->GetId());
        EntityStateCommand::Capture(pSourceEntity, true);
    }

    void EntityDeleteCommand::Undo()
    {
        RestoreEntity(m_undoState.data(), m_undoState.size());
    }

    void EntityDeleteCommand::Redo()
    {
        EBUS_EVENT(AZ::ComponentApplicationBus, DeleteEntity, m_entityID);
        PreemptiveUndoCache::Get()->PurgeCache(m_entityID);
    }

    EntityCreateCommand::EntityCreateCommand(UndoSystem::URCommandID ID)
        : EntityStateCommand(ID, "Create Entity")
    {
    }

    void EntityCreateCommand::Capture(AZ::Entity* pSourceEntity)
    {
        EntityStateCommand::Capture(pSourceEntity, false);
        m_isSelected = true;
    }

    void EntityCreateCommand::Undo()
    {
        EBUS_EVENT(AZ::ComponentApplicationBus, DeleteEntity, m_entityID);
        PreemptiveUndoCache::Get()->PurgeCache(m_entityID);
    }

    void EntityCreateCommand::Redo()
    {
        RestoreEntity(m_redoState.data(), m_redoState.size());
    }
} // namespace AzToolsFramework
