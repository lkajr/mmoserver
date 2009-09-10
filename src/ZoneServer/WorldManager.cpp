/*
---------------------------------------------------------------------------------------
This source file is part of swgANH (Star Wars Galaxies - A New Hope - Server Emulator)
For more information, see http://www.swganh.org


Copyright (c) 2006 - 2009 The swgANH Team

---------------------------------------------------------------------------------------
*/

#include "WorldManager.h"
#include "ZoneOpcodes.h"
#include "Common/MessageDispatch.h"
#include "Common/MessageFactory.h"
#include "Common/Message.h"
#include "Common/DispatchClient.h"
//#include "Utils/clock.h"
#include "DatabaseManager/Database.h"
#include "DatabaseManager/DataBinding.h"
#include "DatabaseManager/DatabaseResult.h"
#include "DatabaseManager/Transaction.h"
#include "LogManager/LogManager.h"
#include "ConfigManager/ConfigManager.h"
#include "TreasuryManager.h"
#include "WorldConfig.h"
#include "ScriptEngine/ScriptEngine.h"
#include "ScriptEngine/Script.h"
#include "ScriptEngine/ScriptSupport.h"
#include "CraftingSessionFactory.h"
#include "CraftingTool.h"
#include "EntertainerManager.h"
#include "Buff.h"
#include "BuffManager.h"
#include "MissionManager.h"
#include "Utils/Scheduler.h"
#include "AttackableCreature.h"
#include "NpcManager.h"
#include "GroupManager.h"
#include "Heightmap.h"
#include "CreatureSpawnRegion.h"

//======================================================================================================================

bool			WorldManager::mInsFlag    = false;
WorldManager*	WorldManager::mSingleton  = NULL;
//======================================================================================================================

WorldManager::WorldManager(uint32 zoneId,ZoneServer* zoneServer,Database* database) :
mZoneId(zoneId),
mZoneServer(zoneServer),
mDatabase(database),
mTotalObjectCount(0),
mServerTime(0),
mState(WMState_StartUp),
mWM_DB_AsyncPool(sizeof(WMAsyncContainer))
{
	gLogger->logMsg("WorldManager::StartUp\n");

	// set up spatial index
	mSpatialIndex = new ZoneTree();
	mSpatialIndex->Init(gConfig->read<float>("FillFactor"),
						gConfig->read<int>("IndexCap"),
						gConfig->read<int>("LeafCap"),
						2,
						gConfig->read<float>("Horizon"));

	// create schedulers
	mSubsystemScheduler		= new Anh_Utils::Scheduler();
	mObjControllerScheduler = new Anh_Utils::Scheduler();
	mHamRegenScheduler		= new Anh_Utils::Scheduler();
	mPlayerScheduler		= new Anh_Utils::Scheduler();
	mEntertainerScheduler	= new Anh_Utils::Scheduler();
	mBuffScheduler			= new Anh_Utils::VariableTimeScheduler(100, 100);
	mMissionScheduler		= new Anh_Utils::Scheduler();
	mNpcManagerScheduler	= new Anh_Utils::Scheduler();
	

	LoadCurrentGlobalTick();


	// preallocate
	mvClientEffects.reserve(1000);
	mvMoods.reserve(200);
	mvSounds.reserve(5000);
	mPlayersToRemove.reserve(10);
	mShuttleList.reserve(50);

	// load up subsystems

	SkillManager::Init(database);
	SchematicManager::Init(database);
	ResourceManager::Init(database,mZoneId);
	ResourceCollectionManager::Init(database);
	TreasuryManager::Init(database);
	ConversationManager::Init(database);
	CraftingSessionFactory::Init(database);
    MissionManager::Init(database,mZoneId);

	// register world script hooks
	_registerScriptHooks();

	// initiate loading of objects
	mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_ObjectCount),"SELECT sf_getZoneObjectCount(%i);",mZoneId);
}

//======================================================================================================================

WorldManager*	WorldManager::Init(uint32 zoneId,ZoneServer* zoneServer,Database* database)
{
	if(!mInsFlag)
	{
		mSingleton = new WorldManager(zoneId,zoneServer,database);
		mInsFlag = true;
		return mSingleton;
	}
	else
		return mSingleton;
}

//======================================================================================================================

void WorldManager::Shutdown()
{
	// clear scripts
	ScriptList::iterator scriptIt = mWorldScripts.begin();

	while(scriptIt != mWorldScripts.end())
	{
		gScriptEngine->removeScript(*scriptIt);
		scriptIt = mWorldScripts.erase(scriptIt);
	}

	// timers
	delete (mNpcManagerScheduler);
	delete(mSubsystemScheduler);
	delete(mObjControllerScheduler);
	delete(mHamRegenScheduler);
	delete(mMissionScheduler);
	delete(mPlayerScheduler);
	delete(mEntertainerScheduler);
	delete(mBuffScheduler);

	// objects
	mPlayerAccMap.clear();
	mPlayersToRemove.clear();
	mRegionMap.clear();

	// Npc conversation timers.
	mNpcConversionTimers.clear();

	// Player movement update timers.
	mPlayerMovementUpdateMap.clear();

	mCreatureObjectDeletionMap.clear();
	mPlayerObjectReviveMap.clear();

	mNpcDormantHandlers.clear();
	mNpcReadyHandlers.clear();
	mNpcActiveHandlers.clear();

	// Handle creature spawn regions. These objects are not registred in the normal object map.
	CreatureSpawnRegionMap::iterator it = mCreatureSpawnRegionMap.begin();
	while (it != mCreatureSpawnRegionMap.end())
	{
		delete (*it).second;
		it = mCreatureSpawnRegionMap.erase(it);
	}
	mCreatureSpawnRegionMap.clear();

	NpcManager::deleteManager();
	Heightmap::deleter();

	// finally delete them
	mObjectMap.clear();
	mQTRegionMap.clear();

	// shutdown SI
	mSpatialIndex->ShutDown();
	delete(mSpatialIndex);
}

//======================================================================================================================

WorldManager::~WorldManager()
{
	mInsFlag = false;
	delete(mSingleton);
}

//======================================================================================================================

void WorldManager::handleDatabaseJobComplete(void* ref,DatabaseResult* result)
{
	WMAsyncContainer* asyncContainer = reinterpret_cast<WMAsyncContainer*>(ref);

	switch(mState)
	{
		//
		// startup queries
		//
		case WMState_StartUp:
		{
			switch(asyncContainer->mQuery)
			{
				case WMQuery_ObjectCount:
				{
					// we got the total objectCount we need to load
					DataBinding*	binding = mDatabase->CreateDataBinding(1);
					binding->addField(DFT_uint32,0,4);
					result->GetNextRow(binding,&mTotalObjectCount);
	
					gLogger->logMsgStartUp("WorldManager::Loading Objects...");

					if(mTotalObjectCount > 0)
					{
						// this loads all buildings with cells and objects they contain
						_loadBuildings();
						// load objects in world
						_loadAllObjects(0);

						// load zone regions
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_ZoneRegions),"SELECT id FROM zone_regions WHERE planet_id=%u ORDER BY id;",mZoneId);

						// load client effects
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_ClientEffects),"SELECT * FROM clienteffects ORDER BY id;");

						// load planet names and terrain files
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_PlanetNamesAndFiles),"SELECT * FROM planet ORDER BY planet_id;");

						// load attribute keys
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_AttributeKeys),"SELECT * FROM attributes ORDER BY id;");

						// load sounds
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_Sounds),"SELECT * FROM sounds ORDER BY id;");

						// load moods
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_Moods),"SELECT * FROM moods ORDER BY id;");

						// load npc converse animations
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_NpcConverseAnimations),"SELECT * FROM conversation_animations ORDER BY id;");

						// load npc chatter
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_NpcChatter),"SELECT * FROM npc_chatter WHERE planetId=%u OR planetId=99;",mZoneId);

						// load cities
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_Cities),"SELECT id FROM cities WHERE planet_id=%u ORDER BY id;",mZoneId);

						// load badge regions
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_BadgeRegions),"SELECT id FROM badge_regions WHERE planet_id=%u ORDER BY id;",mZoneId);

						//load spawn regions
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_SpawnRegions),"SELECT id FROM spawn_regions WHERE planet_id=%u ORDER BY id;",mZoneId);

						// load world scripts
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_WorldScripts),"SELECT priority,file FROM config_zone_scripts WHERE planet_id=%u ORDER BY id;",mZoneId);

						//load creature spawn regions, and optionally heightmaps cache.
						mDatabase->ExecuteSqlAsync(this,new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_CreatureSpawnRegions),"SELECT id, spawn_x, spawn_z, spawn_width, spawn_length FROM spawns WHERE spawn_planet=%u ORDER BY id;",mZoneId);

						//printf isnt threadsafe ...
						gLogger->logMsgFollowUp(" %u loaded",MSG_NORMAL,mTotalObjectCount);
						//printf();
						gLogger->logMsgOk(20);
					}					
					// no objects to load, so we are done
					else
					{
						_handleLoadComplete();
					}

					mDatabase->DestroyDataBinding(binding);
				}
				break;

				// zone regions
				case WMQuery_ZoneRegions:
				{
					gLogger->logMsgStartUp("WorldManager::Loading zone region...");

					DataBinding* regionBinding = mDatabase->CreateDataBinding(1);
					regionBinding->addField(DFT_int64,0,8);

					uint64 regionId;
					uint64 count = result->getRowCount();

					for(uint64 i = 0;i < count;i++)
					{
						result->GetNextRow(regionBinding,&regionId);

						gObjectFactory->requestObject(ObjType_Region,Region_Zone,0,this,regionId,asyncContainer->mClient);
					}

					gLogger->logMsgFollowUp(" %3lld loaded",MSG_NORMAL,count);
					gLogger->logMsgOk(17);
					mDatabase->DestroyDataBinding(regionBinding);
				}
				break;

				// planet names and according terrain file names
				case WMQuery_PlanetNamesAndFiles:
				{
					string			tmp;
					DataBinding*	nameBinding = mDatabase->CreateDataBinding(1);
					nameBinding->addField(DFT_bstring,0,255,1);

					uint64 rowCount = result->getRowCount();

					for(uint64 i = 0;i < rowCount;i++)
					{
						result->GetNextRow(nameBinding,&tmp);
						mvPlanetNames.push_back(BString(tmp.getAnsi()));
					}

					mDatabase->DestroyDataBinding(nameBinding);

					result->ResetRowIndex();

					DataBinding*	fileBinding = mDatabase->CreateDataBinding(1);
					fileBinding->addField(DFT_bstring,0,255,2);

					for(uint64 i = 0;i < rowCount;i++)
					{
						result->GetNextRow(fileBinding,&tmp);
						mvTrnFileNames.push_back(BString(tmp.getAnsi()));
					}

					mDatabase->DestroyDataBinding(fileBinding);

				}
				break;

				// global attribute lookup map
				case WMQuery_AttributeKeys:
				{
					string			tmp;
					DataBinding*	binding = mDatabase->CreateDataBinding(1);
					binding->addField(DFT_bstring,0,255,1);

					uint64 attributeCount = result->getRowCount();

					for(uint64 i = 0;i < attributeCount;i++)
					{
						result->GetNextRow(binding,&tmp);

						mObjectAttributeKeyMap.insert(std::make_pair(tmp.getCrc(),BString(tmp.getAnsi())));
					}

					
					gLogger->logMsgStartUp("WorldManager::Loaded %lld Attribute Keys",MSG_NORMAL,attributeCount);
					gLogger->logMsgOk(24);
					mDatabase->DestroyDataBinding(binding);
				}
				break;

				// global client effects map
				case WMQuery_ClientEffects:
				{
					string			tmp;
					DataBinding*	binding = mDatabase->CreateDataBinding(1);
					binding->addField(DFT_bstring,0,255,1);

					uint64 effectCount = result->getRowCount();

					for(uint64 i = 0;i < effectCount;i++)
					{
						result->GetNextRow(binding,&tmp);

						mvClientEffects.push_back(BString(tmp.getAnsi()));
					}
					
					gLogger->logMsgStartUp("WorldManager::Loaded %lld Client Effects",MSG_NORMAL,effectCount);
					gLogger->logMsgOk(25);

					mDatabase->DestroyDataBinding(binding);
				}
				break;

				// global sounds map
				case WMQuery_Sounds:
				{
					string			tmp;
					DataBinding*	binding = mDatabase->CreateDataBinding(1);
					binding->addField(DFT_bstring,0,255,1);

					uint64 effectCount = result->getRowCount();

					for(uint64 i = 0;i < effectCount;i++)
					{
						result->GetNextRow(binding,&tmp);

						mvSounds.push_back(BString(tmp.getAnsi()));
					}					

					gLogger->logMsgStartUp("WorldManager::Loaded %lld Sound Effects",MSG_NORMAL,effectCount);
					gLogger->logMsgOk(25);
					mDatabase->DestroyDataBinding(binding);
				}
				break;

				// global moods map
				case WMQuery_Moods:
				{
					string			tmp;
					DataBinding*	binding = mDatabase->CreateDataBinding(1);
					binding->addField(DFT_bstring,0,255,1);

					uint64 effectCount = result->getRowCount();

					for(uint64 i = 0;i < effectCount;i++)
					{
						result->GetNextRow(binding,&tmp);

						mvMoods.push_back(BString(tmp.getAnsi()));
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %lld moods",MSG_NORMAL,effectCount);
					gLogger->logMsgOk(34);
					mDatabase->DestroyDataBinding(binding);
				}
				break;

				// global npc animations map
				case WMQuery_NpcConverseAnimations:
				{
					string			tmp;
					DataBinding*	binding = mDatabase->CreateDataBinding(1);
					binding->addField(DFT_bstring,0,255,1);

					uint64 animCount = result->getRowCount();

					for(uint64 i = 0;i < animCount;i++)
					{
						result->GetNextRow(binding,&tmp);

						mvNpcConverseAnimations.push_back(BString(tmp.getAnsi()));
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %lld Npc Converse Animations",MSG_NORMAL,animCount);
					gLogger->logMsgOk(17);

					mDatabase->DestroyDataBinding(binding);
				}
				break;

				// random npc phrases/animations map
				case WMQuery_NpcChatter:
				{
					string			tmp;
					DataBinding*	binding = mDatabase->CreateDataBinding(1);
					binding->addField(DFT_bstring,0,255,1);

					uint32			animId;
					DataBinding*	animbinding = mDatabase->CreateDataBinding(1);
					animbinding->addField(DFT_uint32,0,4,2);

					uint64 phraseCount = result->getRowCount();

					for(uint64 i = 0;i < phraseCount;i++)
					{
						result->GetNextRow(binding,&tmp);
						result->ResetRowIndex(i);
						result->GetNextRow(animbinding,&animId);

						tmp.convert(BSTRType_Unicode16);

						mvNpcChatter.push_back(std::make_pair(BString(tmp.getUnicode16()),animId));
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %lld Npc Phrases",MSG_NORMAL,phraseCount);
					gLogger->logMsgOk(28);

					mDatabase->DestroyDataBinding(binding);
					mDatabase->DestroyDataBinding(animbinding);
				}
				break;

				// world scripts
				case WMQuery_WorldScripts:
				{
					DataBinding*	scriptBinding = mDatabase->CreateDataBinding(2);
					scriptBinding->addField(DFT_uint32,offsetof(Script,mPriority),4,0);
					scriptBinding->addField(DFT_string,offsetof(Script,mFile),255,1);

					uint64 scriptCount = result->getRowCount();

					for(uint64 i = 0;i < scriptCount;i++)
					{
						Script* script = gScriptEngine->createScript();

						result->GetNextRow(scriptBinding,script);

						mWorldScripts.push_back(script);
					}
					gLogger->logMsgStartUp("WorldManager::Loaded %3lld world scripts",MSG_NORMAL,scriptCount);
					gLogger->logMsgOk(26);
					mDatabase->DestroyDataBinding(scriptBinding);
				}
				break;

				// buildings
				case WMQuery_All_Buildings:
				{
					gLogger->logMsgStartUp("WorldManager::Loading buildings...");

					uint64			buildingCount;
					uint64			buildingId;
					DataBinding*	buildingBinding = mDatabase->CreateDataBinding(1);
					buildingBinding->addField(DFT_int64,0,8);

					buildingCount = result->getRowCount();

					for(uint64 i = 0;i < buildingCount;i++)
					{
						result->GetNextRow(buildingBinding,&buildingId);

						gObjectFactory->requestObject(ObjType_Building,0,0,this,buildingId,asyncContainer->mClient);
					}

					gLogger->logMsgFollowUp(" %4lld loaded",MSG_NORMAL, buildingCount);
					gLogger->logMsgOk(18);
					mDatabase->DestroyDataBinding(buildingBinding);
				}
				break;

				// city regions
				case WMQuery_Cities:
				{
					DataBinding*	cityBinding = mDatabase->CreateDataBinding(1);
					cityBinding->addField(DFT_int64,0,8);

					uint64 cityId;
					uint64 count = result->getRowCount();

					for(uint64 i = 0;i < count;i++)
					{
						result->GetNextRow(cityBinding,&cityId);

						gObjectFactory->requestObject(ObjType_Region,Region_City,0,this,cityId,asyncContainer->mClient);
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %3lld city regions",MSG_NORMAL,count);
					gLogger->logMsgOk(27);
					mDatabase->DestroyDataBinding(cityBinding);
				}
				break;

				// badge regions
				case WMQuery_BadgeRegions:
				{
					DataBinding*	badgeBinding = mDatabase->CreateDataBinding(1);
					badgeBinding->addField(DFT_int64,0,8);

					uint64 badgeId;
					uint64 count = result->getRowCount();

					for(uint64 i = 0;i < count;i++)
					{
						result->GetNextRow(badgeBinding,&badgeId);

						gObjectFactory->requestObject(ObjType_Region,Region_Badge,0,this,badgeId,asyncContainer->mClient);
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %2lld badge regions",MSG_NORMAL,count);
					gLogger->logMsgOk(27);
					mDatabase->DestroyDataBinding(badgeBinding);
				}
				break;

				// spawn regions
				case WMQuery_SpawnRegions:
				{
					DataBinding*	spawnBinding = mDatabase->CreateDataBinding(1);
					spawnBinding->addField(DFT_int64,0,8);

					uint64 regionId;
					uint64 count = result->getRowCount();

					for(uint64 i = 0;i < count;i++)
					{
						result->GetNextRow(spawnBinding,&regionId);

						gObjectFactory->requestObject(ObjType_Region,Region_Spawn,0,this,regionId,asyncContainer->mClient);
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %4lld spawn regions",MSG_NORMAL,count);
					gLogger->logMsgOk(25);
					mDatabase->DestroyDataBinding(spawnBinding);
				}
				break;

				// Creature spawn regions
				case WMQuery_CreatureSpawnRegions:
				{
					DataBinding*	creatureSpawnBinding = mDatabase->CreateDataBinding(5);
					creatureSpawnBinding->addField(DFT_int64,offsetof(CreatureSpawnRegion,mId),8,0);
					creatureSpawnBinding->addField(DFT_float,offsetof(CreatureSpawnRegion,mPosX),4,1);
					creatureSpawnBinding->addField(DFT_float,offsetof(CreatureSpawnRegion,mPosZ),4,2);
					creatureSpawnBinding->addField(DFT_float,offsetof(CreatureSpawnRegion,mWidth),4,3);
					creatureSpawnBinding->addField(DFT_float,offsetof(CreatureSpawnRegion,mLength),4,4);

					uint64 count = result->getRowCount();

					for(uint64 i = 0;i < count;i++)
					{
						CreatureSpawnRegion *creatureSpawnRegion = new CreatureSpawnRegion();
						result->GetNextRow(creatureSpawnBinding,creatureSpawnRegion);
						mCreatureSpawnRegionMap.insert(std::make_pair(creatureSpawnRegion->mId,creatureSpawnRegion));
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %I64u creature spawn regions.", MSG_NORMAL,count);
					gLogger->logMsgOk(15);
					mDatabase->DestroyDataBinding(creatureSpawnBinding);
				}
				break;

				// container->child objects
				case WMQuery_AllObjectsChildObjects:
				{
					WMQueryContainer queryContainer;

					DataBinding*	binding = mDatabase->CreateDataBinding(2);
					binding->addField(DFT_bstring,offsetof(WMQueryContainer,mString),64,0);
					binding->addField(DFT_uint64,offsetof(WMQueryContainer,mId),8,1);
					
					uint64 count = result->getRowCount();

					for(uint32 i = 0;i < count;i++)
					{
						result->GetNextRow(binding,&queryContainer);

						// now to the ugly part
						if(strcmp(queryContainer.mString.getAnsi(),"terminals") == 0)	
							gObjectFactory->requestObject(ObjType_Tangible,TanGroup_Terminal,0,this,queryContainer.mId,asyncContainer->mClient);
						else if(strcmp(queryContainer.mString.getAnsi(),"containers") == 0)
							gObjectFactory->requestObject(ObjType_Tangible,TanGroup_Container,0,this,queryContainer.mId,asyncContainer->mClient);
						else if(strcmp(queryContainer.mString.getAnsi(),"ticket_collectors") == 0)
							gObjectFactory->requestObject(ObjType_Tangible,TanGroup_TicketCollector,0,this,queryContainer.mId,asyncContainer->mClient);
						else if(strcmp(queryContainer.mString.getAnsi(),"persistent_npcs") == 0)
							gObjectFactory->requestObject(ObjType_NPC,CreoGroup_PersistentNpc,0,this,queryContainer.mId,asyncContainer->mClient);
						else if(strcmp(queryContainer.mString.getAnsi(),"shuttles") == 0)
							gObjectFactory->requestObject(ObjType_Creature,CreoGroup_Shuttle,0,this,queryContainer.mId,asyncContainer->mClient);
						else if(strcmp(queryContainer.mString.getAnsi(),"items") == 0)
							gObjectFactory->requestObject(ObjType_Tangible,TanGroup_Item,0,this,queryContainer.mId,asyncContainer->mClient);
						else if(strcmp(queryContainer.mString.getAnsi(),"resource_containers") == 0)
							gObjectFactory->requestObject(ObjType_Tangible,TanGroup_ResourceContainer,0,this,queryContainer.mId,asyncContainer->mClient);
					}

					gLogger->logMsgStartUp("WorldManager::Loaded %I64u cell children",MSG_NORMAL,count);
					gLogger->logMsgOk(20);
					mDatabase->DestroyDataBinding(binding);
				}
				break;

				default: break;
			}
		}
		break;

		//
		// Queries in running state
		//
		case WMState_Running:
		{
			switch(asyncContainer->mQuery)
			{

				// TODO: make stored function for saving
				case WMQuery_SavePlayer_Position:
				{
					WMAsyncContainer* asyncContainer2	= new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_SavePlayer_Attributes);
					PlayerObject* playerObject			= dynamic_cast<PlayerObject*>(asyncContainer->mObject);
					Ham* ham							= playerObject->getHam();

					if(asyncContainer->mBool)
					{
						asyncContainer2->mBool = true;
					}

					asyncContainer2->mObject		= asyncContainer->mObject;
					asyncContainer2->clContainer	= asyncContainer->clContainer;
					asyncContainer2->mLogout		= asyncContainer->mLogout;

					mDatabase->ExecuteSqlAsync(this,asyncContainer2,"UPDATE character_attributes SET health_current=%u,action_current=%u,mind_current=%u"
						",health_wounds=%u,strength_wounds=%u,constitution_wounds=%u,action_wounds=%u,quickness_wounds=%u"
						",stamina_wounds=%u,mind_wounds=%u,focus_wounds=%u,willpower_wounds=%u,battlefatigue=%u,posture=%u,moodId=%u,title=\'%s\'"
						",character_flags=%u,states=%lld,language=%u,new_player_exemptions=%u WHERE character_id=%lld"
						,ham->mHealth.getCurrentHitPoints(),ham->mAction.getCurrentHitPoints(),ham->mMind.getCurrentHitPoints()
						,ham->mHealth.getWounds(),ham->mStrength.getWounds()
						,ham->mConstitution.getWounds(),ham->mAction.getWounds(),ham->mQuickness.getWounds(),ham->mStamina.getWounds(),ham->mMind.getWounds()
						,ham->mFocus.getWounds(),ham->mWillpower.getWounds(),ham->getBattleFatigue(),playerObject->getPosture(),playerObject->getMoodId(),playerObject->getTitle().getAnsi()
						,playerObject->getPlayerFlags(),playerObject->getState(),playerObject->getLanguage(),playerObject->getNewPlayerExemptions(),playerObject->getId());
				}
				break;

				case WMQuery_SavePlayer_Attributes:
				{
					if(asyncContainer->mBool)
					{
						destroyObject(asyncContainer->mObject);

						//are we to load a new character???
						if(asyncContainer->mLogout == WMLogOut_Char_Load)
						{							
							gObjectFactory->requestObject(ObjType_Player,0,0,asyncContainer->clContainer->ofCallback,asyncContainer->clContainer->mPlayerId,asyncContainer->clContainer->mClient);

							//now destroy the asyncContainer->clContainer
							SAFE_DELETE(asyncContainer->clContainer);
		
						}
						
					}
					if(asyncContainer->mLogout == WMLogOut_Zone_Transfer)
					{							
						//update the position to the new planets position
						Anh_Math::Vector3		destination = asyncContainer->clContainer->destination;
						
						//in this case we retain the asynccontainer and let it be destroyed by the clientlogin handler
						mDatabase->ExecuteSqlAsync(asyncContainer->clContainer->dbCallback,asyncContainer->clContainer,"UPDATE characters SET parent_id=0,x='%f', y='%f', z='%f', planet_id='%u' WHERE id='%I64u';", destination.mX, destination.mY, destination.mZ, asyncContainer->clContainer->planet, asyncContainer->clContainer->player->getId());
					}
				}
				break;

				default:break;
			}
		}
		break;

		//
		// queries when shutting down
		//
		case WMState_ShutDown:
		{

		}
		break;

		default:
			gLogger->logMsgF("WorldManager::DatabaseCallback: unknown state: %i",MSG_HIGH,mState);
		break;
	}

	mWM_DB_AsyncPool.ordered_free(asyncContainer);
}

//======================================================================================================================

void WorldManager::_loadBuildings()
{
	WMAsyncContainer* asynContainer = new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_All_Buildings);

	mDatabase->ExecuteSqlAsync(this,asynContainer,"SELECT id FROM buildings WHERE planet_id = %u;",mZoneId);
}

//======================================================================================================================

void WorldManager::_loadAllObjects(uint64 parentId)
{
	int8	sql[2048];
	WMAsyncContainer* asynContainer = new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_AllObjectsChildObjects);

	sprintf(sql,"(SELECT \'terminals\',terminals.id FROM terminals INNER JOIN terminal_types ON (terminals.terminal_type = terminal_types.id)"
				" WHERE (terminal_types.name NOT LIKE 'unknown') AND (terminals.parent_id = %lld) AND (terminals.planet_id = %u))"
				" UNION (SELECT \'containers\',containers.id FROM containers INNER JOIN container_types ON (containers.container_type = container_types.id)"
				" WHERE (container_types.name NOT LIKE 'unknown') AND (containers.parent_id = %lld) AND (containers.planet_id = %u))"
				" UNION (SELECT \'ticket_collectors\',ticket_collectors.id FROM ticket_collectors WHERE (parent_id=%lld) AND (planet_id=%u))"
				" UNION (SELECT \'persistent_npcs\',persistent_npcs.id FROM persistent_npcs WHERE (parentId=%lld) AND (planet_id = %u))"
				" UNION (SELECT \'shuttles\',shuttles.id FROM shuttles WHERE (parentId=%lld) AND (planet_id = %u))"
				" UNION (SELECT \'items\',items.id FROM items WHERE (parent_id=%lld) AND (planet_id = %u))"
				" UNION (SELECT \'resource_containers\',resource_containers.id FROM resource_containers WHERE (parent_id=%lld) AND (planet_id = %u))",
				parentId,mZoneId,parentId,mZoneId,parentId,mZoneId,parentId,mZoneId,parentId
				,mZoneId,parentId,mZoneId,parentId,mZoneId);

	mDatabase->ExecuteSqlAsync(this,asynContainer,sql);
}

//======================================================================================================================

void WorldManager::handleObjectReady(Object* object,DispatchClient* client)
{
	if(QTRegion* region = dynamic_cast<QTRegion*>(object))
	{
		uint32 key = (uint32)region->getId();

		mQTRegionMap.insert(key,region);

		mSpatialIndex->insertQTRegion(key,region->mPosition.mX,region->mPosition.mZ,region->getWidth(),region->getHeight());
	}
	else
	{
		addObject(object);
	}

	// check if we done loading
	if ((mState == WMState_StartUp) && (mObjectMap.size() + mQTRegionMap.size() + mCreatureSpawnRegionMap.size() >= mTotalObjectCount))
	{
		_handleLoadComplete();
	}
}

//======================================================================================================================

RegionObject* WorldManager::getRegionById(uint64 regionId)
{
	RegionMap::iterator it = mRegionMap.find(regionId);

	if(it != mRegionMap.end())
		return((*it).second);
	else
		gLogger->logMsgF("Worldmanager::getRegionById: Could not find region %lld\n",MSG_NORMAL,regionId);

	return(NULL);
}

//======================================================================================================================

void  WorldManager::initPlayersInRange(Object* object,PlayerObject* player)
{
	// we still query for players here, cause they are found through the buildings and arent kept in a qtree
	ObjectSet inRangeObjects;
	mSpatialIndex->getObjectsInRange(object,&inRangeObjects,(ObjType_Player),gWorldConfig->getPlayerViewingRange());
	
	// query the according qtree, if we are in one
	if(object->getSubZoneId())
	{
		if(QTRegion* region = getQTRegion(object->getSubZoneId()))
		{
			float				viewingRange	= (float)gWorldConfig->getPlayerViewingRange();
			Anh_Math::Rectangle qRect;

			if(!object->getParentId())
			{
				qRect = Anh_Math::Rectangle(object->mPosition.mX - viewingRange,object->mPosition.mZ - viewingRange,viewingRange * 2,viewingRange * 2);
			}
			else
			{
				CellObject*		cell		= dynamic_cast<CellObject*>(getObjectById(object->getParentId()));
				BuildingObject* building	= dynamic_cast<BuildingObject*>(getObjectById(cell->getParentId()));

				qRect = Anh_Math::Rectangle(building->mPosition.mX - viewingRange,building->mPosition.mZ - viewingRange,viewingRange * 2,viewingRange * 2);
			}

			region->mTree->getObjectsInRange(object,&inRangeObjects,ObjType_Player,&qRect);
		}
	}

	// iterate through the results
	ObjectSet::iterator it = inRangeObjects.begin(); 

	while(it != inRangeObjects.end())
	{
		PlayerObject* pObject = dynamic_cast<PlayerObject*>(*it);

		if(pObject)
		{
			if(pObject != player)
			{
				gMessageLib->sendCreateObject(object,pObject);
				pObject->addKnownObjectSafe(object);
				object->addKnownObjectSafe(pObject);
			}
	
		}

		++it;
	}

}

//======================================================================================================================

void WorldManager::initObjectsInRange(PlayerObject* playerObject)
{
	// we still query for players here, cause they are found through the buildings and arent kept in a qtree
	ObjectSet inRangeObjects;
	mSpatialIndex->getObjectsInRange(playerObject,&inRangeObjects,(ObjType_Player | ObjType_Tangible | ObjType_NPC | ObjType_Creature | ObjType_Building),gWorldConfig->getPlayerViewingRange());
	
	// query the according qtree, if we are in one
	if(playerObject->getSubZoneId())
	{
		if(QTRegion* region = getQTRegion(playerObject->getSubZoneId()))
		{
			float				viewingRange	= (float)gWorldConfig->getPlayerViewingRange();
			Anh_Math::Rectangle qRect;

			if(!playerObject->getParentId())
			{
				qRect = Anh_Math::Rectangle(playerObject->mPosition.mX - viewingRange,playerObject->mPosition.mZ - viewingRange,viewingRange * 2,viewingRange * 2);
			}
			else
			{
				CellObject*		cell		= dynamic_cast<CellObject*>(getObjectById(playerObject->getParentId()));
				BuildingObject* building	= dynamic_cast<BuildingObject*>(getObjectById(cell->getParentId()));

				qRect = Anh_Math::Rectangle(building->mPosition.mX - viewingRange,building->mPosition.mZ - viewingRange,viewingRange * 2,viewingRange * 2);
			}

			region->mTree->getObjectsInRange(playerObject,&inRangeObjects,ObjType_Player,&qRect);
		}
	}

	// iterate through the results
	ObjectSet::iterator it = inRangeObjects.begin(); 

	while(it != inRangeObjects.end())
	{
		Object* object = (*it);

		// send create for the type of object
		if (object->getPrivateOwner()) 
		{
			if (object->isOwnedBy(playerObject))
			{
				gMessageLib->sendCreateObject(object,playerObject);
				object->addKnownObjectSafe(playerObject);
				playerObject->addKnownObjectSafe(object);
			}
		}
		else
		{
			gMessageLib->sendCreateObject(object,playerObject);
			object->addKnownObjectSafe(playerObject);
			playerObject->addKnownObjectSafe(object);
		}
		++it;
	}
}

//======================================================================================================================

void WorldManager::savePlayer(uint32 accId,bool remove, WMLogOut mLogout, CharacterLoadingContainer* clContainer)
{
	PlayerObject* playerObject			= getPlayerByAccId(accId);

	// WMQuery_SavePlayer_Position is the query handler called by the buffmanager when all the buffcallbacks are finished
	// we prepare the asynccontainer here already
	WMAsyncContainer* asyncContainer	= new(mWM_DB_AsyncPool.ordered_malloc()) WMAsyncContainer(WMQuery_SavePlayer_Position);

	if(remove)
	{
		asyncContainer->mBool = true;
	}

	//clarify what handler we have to call after saving - if any
	asyncContainer->mObject			= playerObject;
	asyncContainer->mLogout			=   mLogout;
	asyncContainer->clContainer		=	clContainer;

	//start by saving the buffs the buffmanager will deal with the buffspecific db callbacks and start the position safe at their end
	//which will return its callback to the worldmanager

	//if no buff was there to be saved we will continue directly
	if(!gBuffManager->SaveBuffsAsync(asyncContainer, this, playerObject, GetCurrentGlobalTick()))
	{

		// position save will be called by the buff callback if there is any buff
		mDatabase->ExecuteSqlAsync(this,asyncContainer,"UPDATE characters SET parent_id=%lld,oX=%f,oY=%f,oZ=%f,oW=%f,x=%f,y=%f,z=%f,planet_id=%u,jedistate=%u WHERE id=%lld",playerObject->getParentId()
							,playerObject->mDirection.mX,playerObject->mDirection.mY,playerObject->mDirection.mZ,playerObject->mDirection.mW
							,playerObject->mPosition.mX,playerObject->mPosition.mY,playerObject->mPosition.mZ
							,mZoneId,playerObject->getJediState(),playerObject->getId());
	}
	
	
}

//======================================================================================================================

void WorldManager::savePlayerSync(uint32 accId,bool remove)
{
	PlayerObject* playerObject = getPlayerByAccId(accId);
	Ham* ham = playerObject->getHam();

	mDatabase->DestroyResult(mDatabase->ExecuteSynchSql("UPDATE characters SET parent_id=%lld,oX=%f,oY=%f,oZ=%f,oW=%f,x=%f,y=%f,z=%f,planet_id=%u WHERE id=%lld",playerObject->getParentId()
						,playerObject->mDirection.mX,playerObject->mDirection.mY,playerObject->mDirection.mZ,playerObject->mDirection.mW
						,playerObject->mPosition.mX,playerObject->mPosition.mY,playerObject->mPosition.mZ
						,mZoneId,playerObject->getId()));

	mDatabase->DestroyResult(mDatabase->ExecuteSynchSql("UPDATE character_attributes SET health_current=%u,action_current=%u,mind_current=%u"
								",health_wounds=%u,strength_wounds=%u,constitution_wounds=%u,action_wounds=%u,quickness_wounds=%u"
								",stamina_wounds=%u,mind_wounds=%u,focus_wounds=%u,willpower_wounds=%u,battlefatigue=%u,posture=%u,moodId=%u,title=\'%s\'"
								",character_flags=%u,states=%lld,language=%u, group_id=%lld WHERE character_id=%lld",
								ham->mHealth.getCurrentHitPoints() - ham->mHealth.getModifier(), //Llloydyboy Added the -Modifier so that when buffs are reinitialised, it doesn't screw up HAM
								ham->mAction.getCurrentHitPoints() - ham->mAction.getModifier(), //Llloydyboy Added the -Modifier so that when buffs are reinitialised, it doesn't screw up HAM
								ham->mMind.getCurrentHitPoints() - ham->mMind.getModifier(),	 //Llloydyboy Added the -Modifier so that when buffs are reinitialised, it doesn't screw up HAM
								ham->mHealth.getWounds(),
								ham->mStrength.getWounds(),
								ham->mConstitution.getWounds(),
								ham->mAction.getWounds(),
								ham->mQuickness.getWounds(),
								ham->mStamina.getWounds(),
								ham->mMind.getWounds(),
								ham->mFocus.getWounds(),
								ham->mWillpower.getWounds(),
								ham->getBattleFatigue(),
								playerObject->getPosture(),
								playerObject->getMoodId(),
								playerObject->getTitle().getAnsi(),
								playerObject->getPlayerFlags(),
								playerObject->getState(),
								playerObject->getLanguage(),
								playerObject->getGroupId(),
								playerObject->getId()));
	
	
	gBuffManager->SaveBuffs(playerObject, GetCurrentGlobalTick());

	if(remove)
		destroyObject(playerObject);
}

//======================================================================================================================
//get the current tick 
//

uint64 WorldManager::GetCurrentGlobalTick()
{
	return mTick;
}

//======================================================================================================================
//still synch issues to adress with other servers
//

void WorldManager::LoadCurrentGlobalTick()
{
	uint64 Tick;
	DatabaseResult* temp = mDatabase->ExecuteSynchSql("SELECT Global_Tick_Count FROM galaxy WHERE galaxy_id = '2'");
	
	DataBinding*	tickbinding = mDatabase->CreateDataBinding(1);
	tickbinding->addField(DFT_uint64,0,8,0);
	
	temp->GetNextRow(tickbinding,&Tick);
	mDatabase->DestroyDataBinding(tickbinding);
	mDatabase->DestroyResult(temp);

	char strtemp[100];
	sprintf(strtemp, "Current Global Tick Count = %u\n",Tick);
	gLogger->logMsg(strtemp, FOREGROUND_GREEN);
	mTick = Tick;
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleTick),7,1000,NULL);
}

//======================================================================================================================
//
//

bool	WorldManager::_handleTick(uint64 callTime,void* ref)
{
	mTick += 1000;
	return true;
}

//======================================================================================================================
//
//

Object*	WorldManager::getObjectById(uint64 objId)
{
	ObjectMap::iterator it = mObjectMap.find(objId);

	if(it != mObjectMap.end())
	{
		return((*it).second);
	}

	return(NULL);
}

//======================================================================================================================

PlayerObject*	WorldManager::getPlayerByAccId(uint32 accId)
{
	PlayerAccMap::iterator it = mPlayerAccMap.find(accId);

	if(it != mPlayerAccMap.end())
	{
		return(PlayerObject*)((*it).second);
	}

	return(NULL);
}

//======================================================================================================================

void WorldManager::addDisconnectedPlayer(PlayerObject* playerObject)
{
	//uint32 timeOut = gWorldConfig->getLoggedTime();
	uint32 timeOut = gWorldConfig->getConfiguration("Zone_Player_Logout",300);

	gLogger->logMsgF("Player(%lld) disconnected,reconnect timeout in %u seconds\n",MSG_NORMAL,playerObject->getId(),timeOut);

	// Halt the tutorial scripts, if running.
	playerObject->stopTutorial();

	// Delete private owned spawned objects, like npc's in the Tutorial.
	uint64 privateOwnedObjectId = ScriptSupport::Instance()->getObjectOwnedBy(playerObject->getId());
	while (privateOwnedObjectId != 0)
	{
		// Delete the object ref from script support.
		ScriptSupport::Instance()->eraseObject(privateOwnedObjectId);

		// We did have a private npc. Let us delete him/her/that.
		Object* object = this->getObjectById(privateOwnedObjectId);
		if (object)
		{
			// But first, remove npc from our defender list.
			// playerObject->removeDefender(object);
			playerObject->removeDefenderAndUpdateList(object->getId());

			this->destroyObject(object);
			// gLogger->logMsgF("WorldManager::addDisconnectedPlayer Deleted object with id  %llu",MSG_NORMAL,privateOwnedObjectId);
		}
		privateOwnedObjectId = ScriptSupport::Instance()->getObjectOwnedBy(playerObject->getId());
	}

	removeObjControllerToProcess(playerObject->getController()->getTaskId());
	removeCreatureHamToProcess(playerObject->getHam()->getTaskId());
	removeEntertainerToProcess(playerObject->getEntertainerTaskId());

	gCraftingSessionFactory->destroySession(playerObject->getCraftingSession());
	playerObject->setCraftingSession(NULL);
	playerObject->toggleStateOff(CreatureState_Crafting);
		
	//any speeder out?

	//despawn camps ??? - every reference is over id though

	playerObject->getController()->setTaskId(0);
	playerObject->getHam()->setTaskId(0);
	playerObject->setSurveyState(false);
	playerObject->setSamplingState(false);
	playerObject->togglePlayerFlagOn(PlayerFlag_LinkDead);
	playerObject->setConnectionState(PlayerConnState_LinkDead);
	playerObject->setDisconnectTime(timeOut);
	mPlayersToRemove.push_back(playerObject);

	gMessageLib->sendUpdatePlayerFlags(playerObject);
}

//======================================================================================================================

void WorldManager::addReconnectedPlayer(PlayerObject* playerObject)
{
	uint32 timeOut = gWorldConfig->getConfiguration("Zone_Player_Logout",300);

	playerObject->togglePlayerFlagOff(PlayerFlag_LinkDead);
	playerObject->setConnectionState(PlayerConnState_Connected);

	// Restart the tutorial.
	playerObject->startTutorial();

	playerObject->setDisconnectTime(timeOut);

	// resetting move and tickcounters
	playerObject->setInMoveCount(0);
	playerObject->setClientTickCount(0);

	gLogger->logMsgF("Player(%lld) reconnected\n",MSG_NORMAL,playerObject->getId());

	removePlayerFromDisconnectedList(playerObject);
}

//======================================================================================================================

void WorldManager::removePlayerFromDisconnectedList(PlayerObject* playerObject)
{
	PlayerList::iterator it;

	if((it = std::find(mPlayersToRemove.begin(),mPlayersToRemove.end(),playerObject)) == mPlayersToRemove.end())
	{
		gLogger->logMsgF("WorldManager::addReconnectedPlayer: Error removing Player from Disconnected List: %lld\n",MSG_HIGH,playerObject->getId());
	}
	else
	{
		mPlayersToRemove.erase(it);
	}
}

//======================================================================================================================

void WorldManager::Process()
{
	_processSchedulers();
}

//======================================================================================================================

void WorldManager::_processSchedulers()
{
	mHamRegenScheduler->process();
	mSubsystemScheduler->process();
	mObjControllerScheduler->process();
	mPlayerScheduler->process();
	mEntertainerScheduler->process();
	mBuffScheduler->process();
	mMissionScheduler->process();
	mNpcManagerScheduler->process();
}

//======================================================================================================================

bool WorldManager::_handleDisconnectUpdate(uint64 callTime,void* ref)
{


	PlayerList::iterator it = mPlayersToRemove.begin();
	while(it != mPlayersToRemove.end())
	{
		PlayerObject* playerObject = (*it);

		// we timed out, so save + remove it 
		if(--*(playerObject->getDisconnectTime()) <= 0 && playerObject->isLinkDead())
		{			
			// reset link dead state
			playerObject->togglePlayerFlagOff(PlayerFlag_LinkDead);
			playerObject->setConnectionState(PlayerConnState_Destroying);

			// Stop update timers.
			removePlayerMovementUpdateTime(playerObject);

			//remove the player out of his group - if any
			GroupObject* group = gGroupManager->getGroupObject(playerObject->getGroupId());
			if(group)
				group->removePlayer(playerObject->getId());
		
			//asynch save
			savePlayer(playerObject->getAccountId(),true,WMLogOut_LogOut);
			
			
			it = mPlayersToRemove.erase(it);
		}
		else
			it++;
		
	}
	return(true);
}

//======================================================================================================================

bool WorldManager::_handleShuttleUpdate(uint64 callTime,void* ref)
{
	ShuttleList::iterator shuttleIt = mShuttleList.begin();
	while(shuttleIt != mShuttleList.end())
	{
		Shuttle* shuttle = (*shuttleIt);
		
		// The Ticket Collector need a valid shuttle-object.
		if (!shuttle->ticketCollectorEnabled())
		{
			TicketCollector* collector = dynamic_cast<TicketCollector*>(getObjectById(shuttle->getCollectorId()));
			if (collector)
			{
				if (!collector->getShuttle())
				{
					// Enable the collector.
					collector->setShuttle(shuttle);
				}
				shuttle->ticketCollectorEnable();
			}
		}

		switch(shuttle->getShuttleState())
		{
			case ShuttleState_Away:
			{
				uint32 awayTime = shuttle->getAwayTime() + 1000;
				if(awayTime >= shuttle->getAwayInterval())
				{
					shuttle->setPosture(0);
					shuttle->setAwayTime(0);
					shuttle->setShuttleState(ShuttleState_AboutBoarding);
					
					gMessageLib->sendPostureUpdate(shuttle);
					gMessageLib->sendCombatAction(shuttle,NULL,opChange_Posture);
				}
				else
					shuttle->setAwayTime(awayTime);
			}
			break;

			case ShuttleState_Landing:
			{
				uint32 landingTime = shuttle->getLandingTime() + 1000;

				if(landingTime >= SHUTTLE_LANDING_ANIMATION_TIME - 5000)
				{
					shuttle->setShuttleState(ShuttleState_AboutBoarding);
				}
				else
					shuttle->setLandingTime(landingTime);
			}
			break;

			case ShuttleState_AboutBoarding:
			{
				uint32 landingTime = shuttle->getLandingTime() + 1000;

				if(landingTime >= SHUTTLE_LANDING_ANIMATION_TIME)
				{
					shuttle->setLandingTime(0);

					shuttle->setShuttleState(ShuttleState_InPort);
				}
				else
					shuttle->setLandingTime(landingTime);
			}
			break;

			case ShuttleState_InPort:
			{
				uint32 inPortTime = shuttle->getInPortTime() + 1000;
				if(inPortTime >= shuttle->getInPortInterval())
				{
					shuttle->setInPortTime(0);
					shuttle->setShuttleState(ShuttleState_Away);
					shuttle->setPosture(2);

					gMessageLib->sendPostureUpdate(shuttle);
					gMessageLib->sendCombatAction(shuttle,NULL,opChange_Posture);
				}
				else
				{
					shuttle->setInPortTime(inPortTime);
				}
			}
			break;

			default:break;
		}

		++shuttleIt;
	}

	return(true);
}

//======================================================================================================================
//
// wide range move on the same planet
//

void WorldManager::warpPlanet(PlayerObject* playerObject,Anh_Math::Vector3 destination,uint64 parentId,Anh_Math::Quaternion direction)
{
	// remove player from objects in his range.
	removePlayerMovementUpdateTime(playerObject);

	//remove the player out of his group - if any
	GroupObject* group = gGroupManager->getGroupObject(playerObject->getGroupId());
	
	if(group)
		group->removePlayer(playerObject->getId());

	playerObject->destroyKnownObjects();
	
	// remove from cell / SI
	if(playerObject->getParentId())
	{
		if(CellObject* cell = dynamic_cast<CellObject*>(getObjectById(playerObject->getParentId())))
		{
			cell->removeChild(playerObject);
		}
		else
		{
			gLogger->logMsgF("WorldManager::removePlayer: couldn't find cell %lld\n",MSG_HIGH,playerObject->getParentId());
		}
	}
	else
	{
		if(playerObject->getSubZoneId())
		{
			if(QTRegion* region = getQTRegion(playerObject->getSubZoneId()))
			{
				playerObject->setSubZoneId(0);
				region->mTree->removeObject(playerObject);
			}
		}
	}

	// remove any timers timers running
	removeObjControllerToProcess(playerObject->getController()->getTaskId());
	removeCreatureHamToProcess(playerObject->getHam()->getTaskId());
	playerObject->getController()->clearQueues();
	playerObject->getController()->setTaskId(0);
	playerObject->getHam()->setTaskId(0);
	
	// reset player properties
	playerObject->resetProperties();
		
	playerObject->setParentId(parentId);
	playerObject->mPosition		= destination;
	playerObject->mDirection	= direction;

	// start the new scene
	gMessageLib->sendStartScene(mZoneId,playerObject);
	gMessageLib->sendServerTime(gWorldManager->getServerTime(),playerObject->getClient());
	
	// add us to cell / SI
	if(parentId)
	{
		if(CellObject* cell = dynamic_cast<CellObject*>(getObjectById(parentId)))
		{
			cell->addChild(playerObject);
		}
		else
		{
			gLogger->logMsgF("WorldManager::warpPlanet: couldn't find cell %lld\n",MSG_HIGH,parentId);
		}
	}
	else
	{
		if(QTRegion* region = mSpatialIndex->getQTRegion(playerObject->mPosition.mX,playerObject->mPosition.mZ))
		{
			playerObject->setSubZoneId((uint32)region->getId());
			region->mTree->addObject(playerObject);
		}
		else
		{
			// we should never get here !
			gLogger->logMsg("WorldManager::addObject: could not find zone region in map\n");
			return;
		}
	}

	// initialize objects in range
	initObjectsInRange(playerObject);

	// initialize at new position
	gMessageLib->sendCreatePlayer(playerObject,playerObject);

	// initialize ham regeneration
	playerObject->getHam()->checkForRegen();
}

//======================================================================================================================
//
// update the current planet time
//
bool WorldManager::_handleServerTimeUpdate(uint64 callTime,void* ref)
{
	mServerTime += gWorldConfig->getServerTimeInterval() + gWorldConfig->getServerTimeSpeed();

	PlayerAccMap::iterator playerIt = mPlayerAccMap.begin();
	while(playerIt != mPlayerAccMap.end())
	{
		const PlayerObject* const playerObject = (*playerIt).second;

		if (playerObject->isConnected())
		{
			gMessageLib->sendServerTime(mServerTime,playerObject->getClient());
		}

		++playerIt;
	}

	return(true);
}

//======================================================================================================================
//
// update busy crafting tools, called every 2 seconds
//
bool WorldManager::_handleCraftToolTimers(uint64 callTime,void* ref)
{
	CraftTools::iterator it = mBusyCraftTools.begin();

	while(it != mBusyCraftTools.end())
	{
		CraftingTool*	tool	= (*it);
		PlayerObject*	player	= dynamic_cast<PlayerObject*>(getObjectById(tool->getParentId() - 1)); 
		Item*			item	= tool->getCurrentItem();

		if(player)
		{
			// we done, create the item
			if(!tool->updateTimer(callTime))
			{
				// add it to the world, if it holds an item
				if(item)
				{
					item->setParentId(dynamic_cast<Inventory*>(player->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory))->getId());
					dynamic_cast<Inventory*>(player->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory))->addObject(item);
					gWorldManager->addObject(item,true);

					gMessageLib->sendCreateTangible(item,player);

					tool->setCurrentItem(NULL);
				}
				//in case of logout/in interplanetary travel it will be in the inventory already

				gMessageLib->sendUpdateTimer(tool,player);
				
				it = mBusyCraftTools.erase(it);
				tool->setAttribute("craft_tool_status","@crafting:tool_status_ready");
				mDatabase->ExecuteSqlAsync(0,0,"UPDATE item_attributes SET value='@crafting:tool_status_ready' WHERE item_id=%lld AND attribute_id=18",tool->getId());

				tool->setAttribute("craft_tool_time",boost::lexical_cast<std::string>(tool->getTimer()));
				gWorldManager->getDatabase()->ExecuteSqlAsync(0,0,"UPDATE item_attributes SET value='%i' WHERE item_id=%lld AND attribute_id=%u",tool->getId(),tool->getTimer(),AttrType_CraftToolTime);
				
				continue;
			}
			// update the time display
			gMessageLib->sendUpdateTimer(tool,player);

			tool->setAttribute("craft_tool_time",boost::lexical_cast<std::string>(tool->getTimer()));
			//gLogger->logMsgF("timer : %i",MSG_HIGH,tool->getTimer());
			gWorldManager->getDatabase()->ExecuteSqlAsync(0,0,"UPDATE item_attributes SET value='%i' WHERE item_id=%lld AND attribute_id=%u",tool->getId(),tool->getTimer(),AttrType_CraftToolTime);
		}

		++it;
	}

	return(true);
}

//======================================================================================================================

void WorldManager::addBusyCraftTool(CraftingTool* tool)
{
	mBusyCraftTools.push_back(tool);
}

//======================================================================================================================

void WorldManager::removeBusyCraftTool(CraftingTool* tool)
{
	CraftTools::iterator it = mBusyCraftTools.begin();

	while(it != mBusyCraftTools.end())
	{
		if((*it) == tool)
		{
			mBusyCraftTools.erase(it);
			break;
		}

		++it;
	}
}
//======================================================================================================================
//
// Handle npc conversations that has expired.
//
bool WorldManager::_handleNpcConversionTimers(uint64 callTime,void* ref)
{
	NpcConversionTimers::iterator it = mNpcConversionTimers.begin();
	while (it != mNpcConversionTimers.end())
	{
		// gLogger->logMsgF("WorldManager::_handleNpcConversionTimers: Checking callTime %lld againts %lld",MSG_NORMAL, callTime, (*it).second);
		// Npc timer has expired?
		if (callTime >= ((*it).second)->mInterval)
		{
			// gLogger->logMsg("Calling restorePosition()");
			// Yes, rotate npc back to original position.
			if (PlayerObject* player = dynamic_cast<PlayerObject*>(getObjectById(((*it).second)->mTargetId)))
			{
				if (NPCObject* npc = dynamic_cast<NPCObject*>(getObjectById((*it).first)))
				{
					npc->restorePosition(player);
				}
			}

			// Remove npc from list.
			// gLogger->logMsgF("\nActivated and deleted %lld\n",MSG_NORMAL, ((*it).second)->mTargetId);
			delete ((*it).second);
			it = mNpcConversionTimers.erase(it);
			continue;
		}
		++it;
	}
	return (true);
}

//======================================================================================================================

void WorldManager::addNpcConversation(uint64 interval, NPCObject* npc)
{
	// Remove npc if already in list, we use the interval from last player that invoked conversation.

	NpcConversionTime* target = new NpcConversionTime;

	target->mNpc = npc;
	target->mTargetId = npc->getLastConversationTarget();
	target->mGroupId = 0;
	if (PlayerObject* player = dynamic_cast<PlayerObject*>(getObjectById(npc->getLastConversationTarget())))
	{
		target->mGroupId = player->getGroupId();
	}
	
	if (gWorldConfig->isInstance())
	{
		// We are running in an instance.
		NpcConversionTimers::iterator it = mNpcConversionTimers.begin();
		while (it != mNpcConversionTimers.end())
		{
			// Only erase if it's same player/group that updates AND when running zone as an instance.
			// gLogger->logMsgF("Comp NPC %lld and %lld",MSG_NORMAL, ((*it).first), npc);
			if (((*it).first) == npc->getId())
			{
				if (target->mGroupId)
				{
					// We belong to a group.
					if (target->mGroupId == ((*it).second)->mGroupId)
					{
						// gLogger->logMsgF("Delete (group) %lld",MSG_NORMAL, ((*it).second)->mTargetId);
						delete ((*it).second);
						it = mNpcConversionTimers.erase(it);
						continue;
					}
				}
				else 
				{

					if (target->mTargetId == ((*it).second)->mTargetId)
					{
						delete ((*it).second);
						it = mNpcConversionTimers.erase(it);
						continue;
					}
					else
					{
						// It may be a new instance using this object.
						// gLogger->logMsg("Unknown target.");
					}
				}
			}
			++it;
		}
	}
	else
	{
		// gLogger->logMsg("Not instanced.");
		NpcConversionTimers::iterator it = mNpcConversionTimers.begin();
		while (it != mNpcConversionTimers.end())
		{
			if (((*it).first) == npc->getId())
			{
				// gLogger->logMsgF("Delete %lld",MSG_NORMAL, ((*it).second)->mTargetId);
				delete ((*it).second);
				mNpcConversionTimers.erase(it);
				break;
			}
			++it;
		}
	}
	uint64 expireTime = Anh_Utils::Clock::getSingleton()->getLocalTime();
	target->mInterval = expireTime + interval;
	mNpcConversionTimers.push_back(std::make_pair(npc->getId(), target));
}

//======================================================================================================================
//
// Handle update of player movements. We need to have a consistent update of the world around us,
// even we we are not moving in world.
//

bool WorldManager::_handlePlayerMovementUpdateTimers(uint64 callTime, void* ref)
{
	PlayerMovementUpdateMap::iterator it = mPlayerMovementUpdateMap.begin();

	while (it != mPlayerMovementUpdateMap.end())
	{
		PlayerObject* player = dynamic_cast<PlayerObject*>(getObjectById((*it).first));
		if (player)
		{
			if (player->isConnected())
			{
				// gLogger->logMsgF("WorldManager::_handleObjectMovementUpdateTimers: Checking player update time %lld againts %lld",MSG_NORMAL, callTime, (*it).second);
				//  The timer has expired?
				if (callTime >= ((*it).second))
				{
					// Yes, handle it.
					// gLogger->logMsg("Calling UPDATEPOSITION-bla-ha ()");

					ObjectController* ObjCtl = player->getController();
				
					uint64 next = ObjCtl->playerWorldUpdate(false);
					it = mPlayerMovementUpdateMap.erase(it);
					if (next)
					{
						// Add next scheduled update.
						addPlayerMovementUpdateTime(player, next);
					}
				}
				else
				{
					++it;
				}
			}
			else
			{
				// Remove the disconnected...
				it = mPlayerMovementUpdateMap.erase(it);
			}
		}
		else
		{
			// Remove the disconnected...
			it = mPlayerMovementUpdateMap.erase(it);
		}
	}
	return (true);
}


//======================================================================================================================
// 
//	Add a timer entry for updating of players known objects.
// 

void WorldManager::addPlayerMovementUpdateTime(PlayerObject* player, uint64 when)
{
	uint64 expireTime = Anh_Utils::Clock::getSingleton()->getLocalTime();
	// gLogger->logMsgF("Adding new at %lld",MSG_NORMAL, expireTime + when);
	mPlayerMovementUpdateMap.insert(std::make_pair(player->getId(), expireTime + when));
}

//======================================================================================================================
// 
//	Remove timer entry for player.
// 

void WorldManager::removePlayerMovementUpdateTime(PlayerObject* player)
{
	if (player)
	{
		PlayerMovementUpdateMap::iterator it = mPlayerMovementUpdateMap.find(player->getId());
		while (it != mPlayerMovementUpdateMap.end())
		{
			// Remove the disconnected...
			mPlayerMovementUpdateMap.erase(it);
			it = mPlayerMovementUpdateMap.find(player->getId());
		}
	}
}


//======================================================================================================================
// 
//	Add a timed entry for deletion of dead creature objects.
// 

void WorldManager::addCreatureObjectForTimedDeletion(uint64 creatureId, uint64 when)
{
	uint64 expireTime = Anh_Utils::Clock::getSingleton()->getLocalTime();

	// gLogger->logMsgF("WorldManager::addCreatureObjectForTimedDeletion Adding new at %lld",MSG_NORMAL, expireTime + when);

	CreatureObjectDeletionMap::iterator it = mCreatureObjectDeletionMap.find(creatureId);
	if (it != mCreatureObjectDeletionMap.end())
	{
		// Only remove object if new expire time is earlier than old. (else people can use "lootall" to add 10 new seconds to a corpse forever).
		if (expireTime + when < (*it).second)
		{
			// gLogger->logMsgF("Removing object with id %lld",MSG_NORMAL, creatureId);
			mCreatureObjectDeletionMap.erase(it);
		}
		else
		{
			return;
		}

	}
	// gLogger->logMsgF("Adding new object with id %lld",MSG_NORMAL, creatureId);
	mCreatureObjectDeletionMap.insert(std::make_pair(creatureId, expireTime + when));
}


//======================================================================================================================
// 
//	Add a timed entry for cloning of dead player objects.
// 

void WorldManager::addPlayerObjectForTimedCloning(uint64 playerId, uint64 when)
{
	uint64 expireTime = Anh_Utils::Clock::getSingleton()->getLocalTime();

	mPlayerObjectReviveMap.insert(std::make_pair(playerId, expireTime + when));
}

//======================================================================================================================
// 
//	Remove a timed entry for cloning of dead player objects.
// 

void WorldManager::removePlayerObjectForTimedCloning(uint64 playerId)
{
	PlayerObjectReviveMap::iterator it = mPlayerObjectReviveMap.find(playerId);
	if (it != mPlayerObjectReviveMap.end())
	{
		// Remove player.
		mPlayerObjectReviveMap.erase(it);
	}
}

//======================================================================================================================
//
// Handle delayed deletion of dead creature objects and revive of dead player objects.
//

bool WorldManager::_handleGroupObjectTimers(uint64 callTime, void* ref)
{
	//iterate through all groups and update the missionwaypoints
	GroupList* groupList = gGroupManager->getGroupList();
	GroupList::iterator it = groupList->begin();

	while(it != groupList->end())
	{
		GroupObject* group = (*it);
		gGroupManager->sendGroupMissionUpdate(group);
		it++;
	}

	return (true);
}

//======================================================================================================================
//
// Handle delayed deletion of dead creature objects and revive of dead player objects.
//

bool WorldManager::_handleGeneralObjectTimers(uint64 callTime, void* ref)
{
	CreatureObjectDeletionMap::iterator it = mCreatureObjectDeletionMap.begin();
	while (it != mCreatureObjectDeletionMap.end())
	{
		//  The timer has expired?
		if (callTime >= ((*it).second))
		{
			// Is it a valid object?
			CreatureObject* creature = dynamic_cast<CreatureObject*>(getObjectById((*it).first));
			if (creature)
			{
				// Yes, handle it. We may put up a copy of this npc...
				NpcManager::Instance()->handleExpiredCreature((*it).first);
				this->destroyObject(creature);
				it = mCreatureObjectDeletionMap.erase(it);
			}
			else
			{
				// Remove the invalid object...from this list.
				it = mCreatureObjectDeletionMap.erase(it);
			}
		}
		else
		{
			++it;
		}
	}

	PlayerObjectReviveMap::iterator reviveIt = mPlayerObjectReviveMap.begin();
	while (reviveIt != mPlayerObjectReviveMap.end())
	{
		//  The timer has expired?
		if (callTime >= ((*reviveIt).second))
		{

			PlayerObject* player = dynamic_cast<PlayerObject*>(getObjectById((*reviveIt).first));
			if (player)
			{
				// Yes, handle it.
				// Send the player to closest cloning facility.
				player->cloneAtNearestCloningFacility();

				// The cloning request removes itself from here, have to restart the iteration.
				reviveIt = mPlayerObjectReviveMap.begin();
			}
			else
			{
				// Remove the invalid object...
				reviveIt = mPlayerObjectReviveMap.erase(reviveIt);
			}
		}
		else
		{
			++reviveIt;
		}
	}
	return (true);
}

//======================================================================================================================
//
// update the current planet weather
//
void WorldManager::updateWeather(float cloudX,float cloudY,float cloudZ,uint32 weatherType)
{
	mCurrentWeather.mWeather = weatherType;
	mCurrentWeather.mClouds.mX = cloudX;
	mCurrentWeather.mClouds.mY = cloudY;
	mCurrentWeather.mClouds.mZ = cloudZ;

	gMessageLib->sendWeatherUpdate(mCurrentWeather.mClouds,mCurrentWeather.mWeather);
}

//======================================================================================================================

void WorldManager::handleTimer(uint32 id, void* container)
{

}

//======================================================================================================================
//
// called on startup, after all objects have been loaded
//
void WorldManager::_handleLoadComplete()
{
	// release memory
	mDatabase->releaseResultPoolMemory();
	mDatabase->releaseJobPoolMemory();
	mDatabase->releaseBindingPoolMemory();
	mWM_DB_AsyncPool.release_memory();
	gObjectFactory->releaseAllPoolsMemory();
	gResourceManager->releaseAllPoolsMemory();
	gSchematicManager->releaseAllPoolsMemory();
	gSkillManager->releaseAllPoolsMemory();

	if (!Heightmap::Instance())
	{
		assert(false);
	}

	// create a height-map cashe.
	int16 resolution = 0;
	if (gConfig->keyExists("heightMapResolution"))
	{
		resolution = gConfig->read<int>("heightMapResolution");
	}
	gLogger->logMsgF("WorldManager::_handleLoadComplete heightMapResolution = %d", MSG_NORMAL, resolution);

	if (Heightmap::Instance()->setupCache(resolution))
	{
		gLogger->logMsgF("WorldManager::_handleLoadComplete heigthmap cache setup successfully with resolution %d", MSG_NORMAL, resolution);
	}
	else
	{
		gLogger->logMsgF("WorldManager::_handleLoadComplete heigthmap cache setup FAILED", MSG_NORMAL);
	}

	// register script hooks
	_startWorldScripts();

	gLogger->logMsg("WorldManager::Load complete\n");

	// switch into running state
	mState = WMState_Running;

	// notify zoneserver
	mZoneServer->handleWMReady();
	mTotalObjectCount = 0;

	// initialize timers
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleShuttleUpdate),7,1000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleServerTimeUpdate),9,gWorldConfig->getServerTimeInterval()*1000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleDisconnectUpdate),1,1000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleRegionUpdate),2,2000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleCraftToolTimers),3,1000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleNpcConversionTimers),8,1000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handlePlayerMovementUpdateTimers),4,1000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleGeneralObjectTimers),5,2000,NULL);
	mSubsystemScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleGroupObjectTimers),5,gWorldConfig->getGroupMissionUpdateTime(),NULL);
	

	// Init NPC Manager, will load lairs from the DB.
	(void)NpcManager::Instance();

	// Initialize the queues for NPC-Manager.
	mNpcManagerScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleDormantNpcs),5,2500,NULL);
	mNpcManagerScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleReadyNpcs),5,1000,NULL);
	mNpcManagerScheduler->addTask(fastdelegate::MakeDelegate(this,&WorldManager::_handleActiveNpcs),5,250,NULL);

	// Initialize static creature lairs.


}

//======================================================================================================================

void WorldManager::removeActiveRegion(RegionObject* regionObject)
{
	ActiveRegions::iterator it = mActiveRegions.begin();

	while(it != mActiveRegions.end())
	{
		if((*it) == regionObject)
		{
			mActiveRegions.erase(it);
			break;
		}

		++it;
	}
}

//======================================================================================================================

bool WorldManager::_handleRegionUpdate(uint64 callTime,void* ref)
{
	ActiveRegions::iterator it = mActiveRegions.begin();

	while(it != mActiveRegions.end())
	{
		(*it)->update();
		++it;
	}

	//now delete any camp regions that are due
	RegionDeleteList::iterator itR = mRegionDeleteList.begin();
	while(itR != mRegionDeleteList.end())
	{
		removeActiveRegion((*itR));
		//now remove region entries

		destroyObject(*itR);
		//delete(*itR);
		itR++;  
	}

	mRegionDeleteList.clear();
	return(true);
}

//======================================================================================================================

int32 WorldManager::getPlanetIdByName(string name)
{
	uint8	id = 0;
	name.toLower();

	BStringVector::iterator it = mvPlanetNames.begin();

	while(it != mvPlanetNames.end())
	{
		if(strcmp((*it).getAnsi(),name.getAnsi()) == 0)
			return(id);

		++it;
		id++;
	}

	return(-1);
}

//======================================================================================================================
//
// remove an entertainer from the entertaining scheduler
//

void WorldManager::removeEntertainerToProcess(uint64 taskId)
{
	mEntertainerScheduler->removeTask(taskId);
}

//======================================================================================================================
//
// remove a creature from the ham regeneration scheduler
//

void WorldManager::removeCreatureHamToProcess(uint64 taskId)
{
	mHamRegenScheduler->removeTask(taskId);
}

//======================================================================================================================
//
// remove an object from the object controller scheduler
//

void WorldManager::removeObjControllerToProcess(uint64 taskId)
{
	mObjControllerScheduler->removeTask(taskId);  
}

//======================================================================================================================
//
// returns true if object exist in world manager object list.
//
bool WorldManager::existObject(Object* object)
{
	if (object)
	{
		return (mObjectMap.find(object->getId()) != mObjectMap.end());
	}
	else
	{
		return false;
	}
}

//======================================================================================================================
//
// returns the id of the first object that has a private owner that match the requested one.
//
// This function is not used yet.
uint64 WorldManager::getObjectOwnedBy(uint64 theOwner)
{
	gLogger->logMsgF("WorldManager::getObjectOwnedBy: Invoked\n",MSG_NORMAL);
	ObjectMap::iterator it = mObjectMap.begin();
	uint64 ownerId = 0;

	while (it != mObjectMap.end())
	{
		if ( ((*it).second)->getPrivateOwner() == theOwner)
		{
			ownerId = (*it).first;
			gLogger->logMsgF("WorldManager::getObjectOwnedBy: Found an object with id = %llu",MSG_NORMAL, ownerId);
			break;
		}
		it++;
	}
	return ownerId;
}


//======================================================================================================================
//
// adds an object to the world->to cells and the SI only, use manual if adding to containers, or cells on preload
//

void WorldManager::addObject(Object* object,bool manual)
{
	uint64 key = object->getId();

	//make sure objects arnt added several times!!!!
	if(getObjectById(key))
		return;

	mObjectMap.insert(key,object);

	// if we want to set the parent manually or the object is from the snapshots and not a building, return
	if(manual)
	{
		return;
	}

	if(object->getId() < 0x0000000100000000 && object->getType() != ObjType_Building)
	{
		// check if a crafting station - in that case add
		Item* item = dynamic_cast<Item*> (object);

		if(item)
		{
			if(!(item->getItemFamily() == ItemFamily_CraftingStations)) 
				return;
		}
		else
		{
			return;
		}
	}

	switch(object->getType())
	{
		// player, when a player enters a planet
		case ObjType_Player:
		{
			
			PlayerObject* player = dynamic_cast<PlayerObject*>(object);
			gLogger->logMsgF("New Player: %lld, Total Players on zone : %i\n",MSG_NORMAL,player->getId(),(getPlayerAccMap())->size() + 1);
			// insert into the player map
			mPlayerAccMap.insert(std::make_pair(player->getAccountId(),player));

			// insert into cell
			if(player->getParentId())
			{
				player->setSubZoneId(0);

				if(CellObject* cell = dynamic_cast<CellObject*>(getObjectById(player->getParentId())))
				{
					cell->addChild(player);
				}
				else
				{
					gLogger->logMsgF("WorldManager::addObject: couldn't find cell %lld",MSG_HIGH,player->getParentId());
				}
			}
			// query the rtree for the qt region we are in
			else
			{
				if(QTRegion* region = mSpatialIndex->getQTRegion(player->mPosition.mX,player->mPosition.mZ))
				{
					player->setSubZoneId((uint32)region->getId());
					region->mTree->addObject(player);
				}
				else
				{
					// we should never get here !
					gLogger->logMsg("WorldManager::addObject: could not find zone region in map");
					return;
				}
			}

			// initialize
			initObjectsInRange(player);
			gMessageLib->sendCreatePlayer(player,player);
			
			// add ham to regeneration scheduler
			player->getHam()->updateRegenRates();	// ERU: Note sure if this is needed here.
			player->getHam()->checkForRegen();

			// onPlayerEntered event, notify scripts
			string params;
			params.setLength(sprintf(params.getAnsi(),"%s %s %u",getPlanetNameThis(),player->getFirstName().getAnsi(),mPlayerAccMap.size()));

			mWorldScriptsListener.handleScriptEvent("onPlayerEntered",params);

			// Start player world position update. Used when player don't get any events from client (player not moving).
			// addPlayerMovementUpdateTime(player, 1000);
		}
		break;

		case ObjType_Building:
		{
			BuildingObject* building = dynamic_cast<BuildingObject*>(object);

			mSpatialIndex->InsertRegion(key,building->mPosition.mX,building->mPosition.mZ,building->getWidth(),building->getHeight());
		}
		break;


		case ObjType_Tangible:
		{
			uint64 parentId = object->getParentId();

			if(parentId == 0)
			{
				mSpatialIndex->InsertPoint(key,object->mPosition.mX,object->mPosition.mZ);
			}
			else
			{
				CellObject* cell = dynamic_cast<CellObject*>(getObjectById(parentId));

				if(cell)
					cell->addChild(object);
				else
					gLogger->logMsgF("WorldManager::addObject couldn't find cell %lld",MSG_NORMAL,parentId);
			}
		}
		break;

		// TODO: add moving creatures to qtregions
		case ObjType_NPC:
		case ObjType_Creature:
		case ObjType_Lair:
		{
			CreatureObject* creature = dynamic_cast<CreatureObject*>(object);

			if(creature->getCreoGroup() == CreoGroup_Shuttle)
				mShuttleList.push_back(dynamic_cast<Shuttle*>(creature));

			uint64 parentId = creature->getParentId();

			if(parentId)
			{
				CellObject* cell = dynamic_cast<CellObject*>(getObjectById(parentId));

				if(cell)
					cell->addChild(creature);
				else
					gLogger->logMsgF("WorldManager::addObject: couldn't find cell %lld",MSG_HIGH,parentId);
			}
			else
			{
				
				switch(creature->getCreoGroup())
				{
					// moving creature, add to QT
					case CreoGroup_Vehicle :
					{
						if(QTRegion* region = mSpatialIndex->getQTRegion(creature->mPosition.mX,creature->mPosition.mZ))
						{
							creature->setSubZoneId((uint32)region->getId());
							region->mTree->addObject(creature);
						}
						else
						{
							gLogger->logMsg("WorldManager::addObject: could not find zone region in map for creature");
							return;
						}

					}
					break;

					// still creature, add to SI
					default :
					{
						mSpatialIndex->InsertPoint(key,creature->mPosition.mX,creature->mPosition.mZ);
					}
				}
	
				
			}
		}
		break;

		case ObjType_Region:
		{
			RegionObject* region = dynamic_cast<RegionObject*>(object);

			mRegionMap.insert(std::make_pair(key,region));

			mSpatialIndex->InsertRegion(key,region->mPosition.mX,region->mPosition.mZ,region->getWidth(),region->getHeight());

			if(region->getActive())
				addActiveRegion(region);
		}
		break;

		case ObjType_Intangible:
		{
			gLogger->logMsgF("Object of type ObjType_Intangible UNHANDLED in WorldManager::addObject:",MSG_HIGH);
		}
		break;

		default:
		{
			gLogger->logMsgF("Unhandled ObjectType in WorldManager::addObject: %ld",MSG_HIGH,object->getType());
			// Please, when adding new stufff, at least take the time to add a stub for that type.
			// Better fail always, than have random crashes.
			assert(false);
		}
		break;
	}
}

//======================================================================================================================
//
// returns a qtregion
//

QTRegion* WorldManager::getQTRegion(uint32 id)
{
	QTRegionMap::iterator it = mQTRegionMap.find(id);

	if(it != mQTRegionMap.end())
	{
		return((*it).second);
	}

	return(NULL);
}

//======================================================================================================================
//
// removes an object from the world
//

void WorldManager::destroyObject(Object* object)
{

	switch(object->getType())
	{
		case ObjType_Player:
		{
			PlayerObject* player = dynamic_cast<PlayerObject*>(object);
			
			// make sure we stop entertaining if we are an entertainer
			gEntertainerManager->stopEntertaining(player);

			// delete instanced instrument
			if(uint64 itemId = player->getPlacedInstrumentId())
			{
				if(Item* item = dynamic_cast<Item*>(gWorldManager->getObjectById(itemId)))
				{
					player->getController()->destroyObject(item->getId());
				}
			}
			
			// make sure we are deleted out of entertainer Ticks when entertained
			if(player->getEntertainerWatchToId())
			{
				if(PlayerObject* entertainer = dynamic_cast<PlayerObject*>(getObjectById(player->getEntertainerWatchToId())))
				{
					if(entertainer)
						gEntertainerManager->removeAudience(entertainer,player); 
				}
			}

			if(player->getEntertainerListenToId())
			{
				if(PlayerObject* entertainer = dynamic_cast<PlayerObject*>(getObjectById(player->getEntertainerListenToId())))
				{
					if(entertainer)
						gEntertainerManager->removeAudience(entertainer,player); 
				}
			}

			// make sure we don't leave a craft session open
			gCraftingSessionFactory->destroySession(player->getCraftingSession());
			player->setCraftingSession(NULL);
			player->toggleStateOff(CreatureState_Crafting);
			player->setCraftingStage(0);
			player->setExperimentationFlag(0);

			// remove any timers we got running
			removeObjControllerToProcess(player->getController()->getTaskId());
			removeCreatureHamToProcess(player->getHam()->getTaskId());
			player->getController()->setTaskId(0);
			player->getHam()->setTaskId(0);

			// remove player from movement update timer.
			removePlayerMovementUpdateTime(player);

			//remove the player out of his group - if any
			GroupObject* group = gGroupManager->getGroupObject(player->getGroupId());
			
			if(group)
				group->removePlayer(player->getId());

			// can't zone or logout while in combat
			player->toggleStateOff(CreatureState_Combat);
			player->toggleStateOff(CreatureState_Dizzy);
			player->toggleStateOff(CreatureState_Stunned);
			player->toggleStateOff(CreatureState_Blinded);
			player->toggleStateOff(CreatureState_Intimidated);

			// update duel lists
			PlayerList::iterator duelIt = player->getDuelList()->begin();

			while(duelIt != player->getDuelList()->end())
			{
				if((*duelIt)->checkDuelList(player))
				{
					PlayerObject* duelPlayer = (*duelIt);

					duelPlayer->removeFromDuelList(player);

					gMessageLib->sendUpdatePvpStatus(player,duelPlayer);
					gMessageLib->sendUpdatePvpStatus(duelPlayer,player);
				}

				++duelIt;
			}

			// update defender lists
			ObjectIDList::iterator defenderIt = player->getDefenders()->begin();

			while (defenderIt != player->getDefenders()->end())
			{
				if (CreatureObject* defenderCreature = dynamic_cast<CreatureObject*>(gWorldManager->getObjectById((*defenderIt))))
				{
					// defenderCreature->removeDefender(player);
					defenderCreature->removeDefenderAndUpdateList(player->getId());

					if(PlayerObject* defenderPlayer = dynamic_cast<PlayerObject*>(defenderCreature))
					{
						gMessageLib->sendUpdatePvpStatus(player,defenderPlayer);
					}

					// gMessageLib->sendNewDefenderList(defenderCreature);

					// if no more defenders, clear combat state
					if(!defenderCreature->getDefenders()->size())
					{
						defenderCreature->toggleStateOff(CreatureState_Combat);

						gMessageLib->sendStateUpdate(defenderCreature);
					}
				}

				++defenderIt;
			}

			// destroy known objects
			object->destroyKnownObjects();				

			// remove us from cell / SI
			if(player->getParentId())
			{
				if(CellObject* cell = dynamic_cast<CellObject*>(getObjectById(object->getParentId())))
				{
					cell->removeChild(object);
				}
				else
				{
					gLogger->logMsgF("WorldManager::removePlayer: couldn't find cell %lld\n",MSG_HIGH,object->getParentId());
				}
			}
			else
			{
				if(player->getSubZoneId())
				{
					if(QTRegion* region = getQTRegion(player->getSubZoneId()))
					{
						player->setSubZoneId(0);
						region->mTree->removeObject(player);
					}
				}
			}

			// remove us from active regions we are in
			ObjectSet regions;
			mSpatialIndex->getObjectsInRange(object,&regions,ObjType_Region,20);

			ObjectSet::iterator objListIt = regions.begin();

			while(objListIt != regions.end())
			{
				RegionObject* region = dynamic_cast<RegionObject*>(*objListIt);

				if(region->getActive())
				{
					region->onObjectLeave(object);
				}

				++objListIt;
			}

			// move to the nearest cloning center, if we are incapped or dead
			if(player->getPosture() == CreaturePosture_Incapacitated
			|| player->getPosture() == CreaturePosture_Dead)
			{
				// bring up the clone selection window
				ObjectSet						inRangeBuildings;
				BStringVector					buildingNames;
				std::vector<BuildingObject*>	buildings;
				BuildingObject*					nearestBuilding = NULL;

				mSpatialIndex->getObjectsInRange(player,&inRangeBuildings,ObjType_Building,8192);

				ObjectSet::iterator buildingIt = inRangeBuildings.begin();

				while(buildingIt != inRangeBuildings.end())
				{
					BuildingObject* building = dynamic_cast<BuildingObject*>(*buildingIt);

					// TODO: This code is not working as intended if player dies inside, since buildings use world coordinates and players inside have cell coordinates.
					// Tranformation is needed before the correct distance can be calculated.
					if(building && building->getBuildingFamily() == BuildingFamily_Cloning_Facility)
					{
						if(!nearestBuilding
						|| (nearestBuilding != building && (player->mPosition.distance2D(building->mPosition) < player->mPosition.distance2D(nearestBuilding->mPosition))))
						{
							nearestBuilding = building;
						}
					}

					++buildingIt;
				}

				if(nearestBuilding)
				{
					if(nearestBuilding->getSpawnPoints()->size())
					{
						if(SpawnPoint* sp = nearestBuilding->getRandomSpawnPoint())
						{
							// update the database with the new values
							mDatabase->ExecuteSqlAsync(0,0,"UPDATE characters SET parent_id=%lld,oX=%f,oY=%f,oZ=%f,oW=%f,x=%f,y=%f,z=%f WHERE id=%lld",sp->mCellId
								,sp->mDirection.mX,sp->mDirection.mY,sp->mDirection.mZ,sp->mDirection.mW
								,sp->mPosition.mX,sp->mPosition.mY,sp->mPosition.mZ
								,player->getId());
						}
					}
				}
			}

			// remove us from the player map
			PlayerAccMap::iterator playerAccIt = mPlayerAccMap.find(player->getAccountId());

			if(playerAccIt != mPlayerAccMap.end())
			{
				gLogger->logMsgF("Player left: %lld, Total Players on zone : %i\n",MSG_NORMAL,player->getId(),(getPlayerAccMap())->size() -1);
				mPlayerAccMap.erase(playerAccIt);
			}
			else
			{
				gLogger->logMsgF("WorldManager::destroyObject: error removing from playeraccmap : %u\n",MSG_HIGH,player->getAccountId());
			}

			// onPlayerLeft event, notify scripts
			string params;
			params.setLength(sprintf(params.getAnsi(),"%s %s %u",getPlanetNameThis(),player->getFirstName().getAnsi(),mPlayerAccMap.size()));

			mWorldScriptsListener.handleScriptEvent("onPlayerLeft",params);
			// gLogger->logMsg("WorldManager::destroyObject: Player Client set to NULL");
			delete player->getClient();
			player->setClient(NULL);
		}
		break;
		case ObjType_NPC:
		case ObjType_Creature:
		{
			CreatureObject* creature = dynamic_cast<CreatureObject*>(object);

			// remove any timers we got running
			removeCreatureHamToProcess(creature->getHam()->getTaskId());

			// remove from cell / SI
			if (!object->getParentId())
			{
				// Not all objects-creatures of this type are points.
				if(creature->getSubZoneId())
				{
					if(QTRegion* region = getQTRegion(creature->getSubZoneId()))
					{
						creature->setSubZoneId(0);
						region->mTree->removeObject(creature);
					}
				}
				else
				{
					mSpatialIndex->RemovePoint(object->getId(),object->mPosition.mX,object->mPosition.mZ);
				}
			}
			else
			{
				if(CellObject* cell = dynamic_cast<CellObject*>(getObjectById(object->getParentId())))
				{
					cell->removeChild(object);
				}
				else
				{
					gLogger->logMsgF("WorldManager::destroyObject: couldn't find cell %lld\n",MSG_HIGH,object->getParentId());
				}
			}

			// destroy known objects
			object->destroyKnownObjects();	

			// if its a shuttle, remove it from the shuttle list
			if(creature->getCreoGroup() == CreoGroup_Shuttle)
			{
				ShuttleList::iterator shuttleIt = mShuttleList.begin();
				while(shuttleIt != mShuttleList.end())
				{
					if((*shuttleIt)->getId() == creature->getId())
					{
						mShuttleList.erase(shuttleIt);
						break;
					}

					++shuttleIt;
				}
			}
		}
		break;

		case ObjType_Building:
		{

		}
		break;

		case ObjType_Cell:
		{

		}
		break;

		case ObjType_Tangible:
		{
			if(TangibleObject* tangible = dynamic_cast<TangibleObject*>(object))
			{
				uint64 parentId = object->getParentId();

				if(parentId == 0)
				{
					mSpatialIndex->RemovePoint(object->getId(),object->mPosition.mX,object->mPosition.mZ);
				}
				else
				{
					if(CellObject* cell = dynamic_cast<CellObject*>(getObjectById(parentId)))
					{
						cell->removeChild(object);
					}
					else
					{
						// Well, Tangible can have more kind of parents than just cells or SI. For example players or Inventory.

						// Here we have a method, destroyObject(), that in some cases handles the complete process of deleting a Tangible object,
						// in case of a cell or SI as Parent. Object owned by Player or Inventory have to be handled outisde this method.

						// gLogger->logMsgF("WorldManager::destroyObject couldn't find cell %lld",MSG_NORMAL,parentId);
					}
				}

				// destroy known objects
				object->destroyKnownObjects();	
			}
			else
			{
				gLogger->logMsgF("WorldManager::destroyObject: error removing : %lld\n",MSG_HIGH,object->getId());
			}
		}
		break;

		case ObjType_Region:
		{
			RegionMap::iterator it = mRegionMap.find(object->getId());

			if(it != mRegionMap.end())
			{
				mRegionMap.erase(it);
			}
			else
			{
				gLogger->logMsgF("Worldmanager::destroyObject: Could not find region %lld\n",MSG_NORMAL,object->getId());
			}

			//camp regions are in here, too
			QTRegionMap::iterator itQ = mQTRegionMap.find(object->getId());
			if(itQ != mQTRegionMap.end())
			{
				mQTRegionMap.erase(itQ);
				gLogger->logMsgF("Worldmanager::destroyObject: qt region %lld",MSG_HIGH,object->getId());
			}
			
		}
		break;

		case ObjType_Intangible:
		{
			gLogger->logMsgF("Object of type ObjType_Intangible almost UNHANDLED in WorldManager::destroyObject:",MSG_HIGH);

			// destroy known objects
			object->destroyKnownObjects();	
		}
		break;

		default:
		{
			gLogger->logMsgF("Unhandled ObjectType in WorldManager::destroyObject: %I64u",MSG_HIGH,object->getType());

			// Please, when adding new stufff, at least take the time to add a stub for that type.
			// Better fail always, than have random crashes.
			assert(false);
		}
		break;
	}


	// finally delete it
	ObjectMap::iterator objMapIt = mObjectMap.find(object->getId());

	if(objMapIt != mObjectMap.end())
	{
		mObjectMap.erase(objMapIt);
	}
	else
	{
		gLogger->logMsgF("WorldManager::destroyObject: error removing from objectmap: %llu",MSG_HIGH,object->getId());
	}
}

//======================================================================================================================
//
// get an attribute string value from the global attribute map
//

string WorldManager::getAttributeKey(uint32 keyId)
{
	AttributeKeyMap::iterator it = mObjectAttributeKeyMap.find(keyId);

	if(it != mObjectAttributeKeyMap.end())
		return((*it).second);

	return BString();
}

//======================================================================================================================
//
// get a random npc phrase
//

std::pair<string,uint32> WorldManager::getRandNpcChatter()
{
	
	if(mvNpcChatter.size())
		return(mvNpcChatter[gRandom->getRand()%mvNpcChatter.size()]);
	else
		return(std::make_pair(BString(L"quack"),2));
}

//======================================================================================================================
//
// initialize / start scripts
//

void WorldManager::_startWorldScripts()
{
	gLogger->logMsg("Loading world scripts...\n");

	ScriptList::iterator scriptIt = mWorldScripts.begin();

	while(scriptIt != mWorldScripts.end())
	{
		(*scriptIt)->run();

		++scriptIt;
	}
}

//======================================================================================================================
//
// script callback, lets scripts register themselves for an event
//

void WorldManager::ScriptRegisterEvent(void* script,std::string eventFunction)
{
	mWorldScriptsListener.registerScript(reinterpret_cast<Script*>(script),(int8*)eventFunction.c_str());
}

//======================================================================================================================
//
// send a system message to every player on the planet
// available for scripts
//

void WorldManager::	zoneSystemMessage(std::string message)
{
	string msg = (int8*)message.c_str();

	msg.convert(BSTRType_Unicode16);

	PlayerAccMap::iterator it = mPlayerAccMap.begin();

	while(it != mPlayerAccMap.end())
	{
		const PlayerObject* const player = (*it).second;

		if(player->isConnected())
		{
			gMessageLib->sendSystemMessage((PlayerObject*)player,msg);
		}

		++it;
	}
}

//======================================================================================================================
//
// register our script hooks with the listener
//

void WorldManager::_registerScriptHooks()
{
	mWorldScriptsListener.registerFunction("onPlayerEntered");
	mWorldScriptsListener.registerFunction("onPlayerLeft");
}

//======================================================================================================================

bool WorldManager::addNpId(uint64 id)
{
	if(mUsedTmpIds.find(id) == mUsedTmpIds.end())
	{
		mUsedTmpIds.insert(id);

		return(true);
	}

	return(false);
}

//======================================================================================================================
// Returns true if id not in use.
bool WorldManager::checkdNpId(uint64 id)
{
	NpIdSet::iterator it = mUsedTmpIds.find(id);
	return (it == mUsedTmpIds.end());
}

//======================================================================================================================


bool WorldManager::removeNpId(uint64 id)
{
	NpIdSet::iterator it = mUsedTmpIds.find(id);

	if(it != mUsedTmpIds.end())
	{
		mUsedTmpIds.erase(it);

		return(true);
	}

	return(false);
}

//======================================================================================================================

uint64 WorldManager::getRandomNpId()
{
	//why dont we just increase them ???
	uint64 id;
	int32 watchDogCounter = 10000;
	bool found = false;
	while ((found == false) && (watchDogCounter > 0))
	{
		id = (gRandom->getRand()%1000000) + 422212465065984;
		if (checkdNpId(id))
		{
			// reserve the id
			addNpId(id);
			found = true;
		}
		else
		{
			// We need to some sort of indication of system failure.
			watchDogCounter--;

		}
	}
	
	if (found == false)
	{
		id = 0;
		gLogger->logMsg("WorldManager::getRandomNpId() SYSTEM FAILURE\n");
	}
	return id;

	// return (gRandom->getRand()%1000000 + 422212465065984);
}

//======================================================================================================================

uint64 WorldManager::getRandomNpNpcIdSequence()
{
	// We need two free consequetives (sp?) id's for a NPC since NPC's have an Inventory. :)
	bool done = false;
	uint64 randomNpIdPair = 0;
	uint64 counter = 0;

	while (!done)
	{
		randomNpIdPair = getRandomNpId();
		// gLogger->logMsgF("Got %lld", MSG_NORMAL, randomNpIdPair);
		
		// if (checkdNpId(randomNpIdPair) && checkdNpId(randomNpIdPair + 1))
		// Check if next id is free to use
		if (checkdNpId(randomNpIdPair + 1))
		{
			// Yes, reserve it!
			if (addNpId(randomNpIdPair + 1))
			{
				done = true;
			}
		}
		else
		{
			// Release the current id.
			removeNpId(randomNpIdPair);
		}
		
		if (counter++ > 1000)
		{
			// TODO: Better handling of this...
			gLogger->logMsg("This place gonna blow...\n");
			randomNpIdPair = 0;
			break;
		}
	}
	return randomNpIdPair;
}


//======================================================================================================================
//
// remove a Buff from the Buff scheduler
//

void WorldManager::removeBuffToProcess(uint64 taskId)
{
	mBuffScheduler->removeTask(taskId);
}

uint64 WorldManager::addBuffToProcess(Buff* buff)
{
	//Create a copy of Buff* which can be Destructed when task completes (leaving the original ptr intact)
	Buff** pBuff = &buff;
	Buff* DestructibleBuff = buff;

	//Create Event
	BuffEvent* bEvent = new BuffEvent(buff);

	//Create Callback
	VariableTimeCallback callback = fastdelegate::MakeDelegate(DestructibleBuff,&Buff::Update);

	//Add Callback to Scheduler
	uint64 temp = mBuffScheduler->addTask(callback,1,buff->GetTickLength(),NULL);
	
	//Give Buff the ID from Scheduler
	buff->SetID(temp);
	return temp;	
}


//=========================================================================================
//
//	return true if object are withing range.
//  Both objects need to be inside the same building or both object outside, to be concidered to be in range.
//

bool WorldManager::objectsInRange(uint64 obj1Id, uint64 obj2Id, float range)
{
	bool inRange = true;

	Object* obj1 = dynamic_cast<Object*>(this->getObjectById(obj1Id));
	Object* obj2 = dynamic_cast<Object*>(this->getObjectById(obj2Id));
	if (!obj1 || !obj2)
	{
		inRange = false;
	}
	// We have to be in the same building or outside.
	else if (obj1->getParentId() == obj2->getParentId())
	{
		// In the same cell (or both outside is rarley the case here)
		if (obj1->mPosition.distance2D(obj2->mPosition) > range)
		{
			inRange = false;
		}
	}
	else if ((obj1->getParentId() == 0) || (obj2->getParentId() == 0))
	{
		// One of us are outside.
		inRange = false;
	}
	else
	{
		// We may be in the same building.
		CellObject* obj1Cell = dynamic_cast<CellObject*>(this->getObjectById(obj1->getParentId()));
		CellObject* obj2Cell = dynamic_cast<CellObject*>(this->getObjectById(obj2->getParentId()));
		if (obj1Cell && obj2Cell && (obj1Cell->getParentId() == obj2Cell->getParentId()))
		{
			// In the same building
			if (obj1->mPosition.distance2D(obj2->mPosition) > range)
			{
				// But out of range.
				inRange = false;
			}
		}
		else
		{
			// In different buildings.
			inRange = false;
		}
	}
	return inRange;
}

bool WorldManager::objectsInRange(Anh_Math::Vector3 obj1Position,  uint64 obj1ParentId, uint64 obj2Id, float range)
{
	bool inRange = true;

	// Object* obj1 = dynamic_cast<Object*>(this->getObjectById(obj1Id));
	Object* obj2 = dynamic_cast<Object*>(this->getObjectById(obj2Id));

	// We have to be in the same building or outside.
	if (obj1ParentId == obj2->getParentId())
	{
		// In the same cell (or both outside is rarley the case here)
		if (obj1Position.distance2D(obj2->mPosition) > range)
		{
			inRange = false;
		}
	}
	else if ((obj1ParentId == 0) || (obj2->getParentId() == 0))
	{
		// One of us are outside.
		inRange = false;
	}
	else
	{
		// We may be in the same building.
		CellObject* obj1Cell = dynamic_cast<CellObject*>(this->getObjectById(obj1ParentId));
		CellObject* obj2Cell = dynamic_cast<CellObject*>(this->getObjectById(obj2->getParentId()));
		if (obj1Cell && obj2Cell && (obj1Cell->getParentId() == obj2Cell->getParentId()))
		{
			// In the same building
			if (obj1Position.distance2D(obj2->mPosition) > range)
			{
				// But out of range.
				inRange = false;
			}
		}
		else
		{
			// In different buildings.
			inRange = false;
		}
	}
	return inRange;
}




//======================================================================================================================
// 
//	Add a npc to the Dormant queue.
// 

void WorldManager::addDormantNpc(uint64 creature, uint64 when)
{
	// gLogger->logMsgF("Adding dormant NPC handler... %llu", MSG_NORMAL, creature);

	uint64 expireTime = Anh_Utils::Clock::getSingleton()->getLocalTime();
	mNpcDormantHandlers.insert(std::make_pair(creature, expireTime + when));
}

//======================================================================================================================
// 
//	Remove a npc from the Dormant queue.
// 

void WorldManager::removeDormantNpc(uint64 creature)
{
	NpcDormantHandlers::iterator it = mNpcDormantHandlers.find(creature);
	if (it != mNpcDormantHandlers.end())
	{
		// Remove creature.
		mNpcDormantHandlers.erase(it);
	}
}

//======================================================================================================================
// 
//	Force a npc from the Dormant queue to be handled at next tick.
// 

void WorldManager::forceHandlingOfDormantNpc(uint64 creature)
{
	NpcDormantHandlers::iterator it = mNpcDormantHandlers.find(creature);
	if (it != mNpcDormantHandlers.end())
	{
		// Change the event time to NOW.
		uint64 now = Anh_Utils::Clock::getSingleton()->getLocalTime();
		(*it).second = now;
	}
}
//======================================================================================================================
//
// Handle the queue of Dormant npc's.
//

bool WorldManager::_handleDormantNpcs(uint64 callTime, void* ref)
{
	NpcDormantHandlers::iterator it = mNpcDormantHandlers.begin();
	while (it != mNpcDormantHandlers.end())
	{
		//  The timer has expired?
		if (callTime >= ((*it).second))
		{
			// Yes, handle it.
			NPCObject* npc = dynamic_cast<NPCObject*>(this->getObjectById((*it).first));
			if (npc)
			{
				// uint64 waitTime = NpcManager::Instance()->handleDormantNpc(creature, callTime - (*it).second);
				// gLogger->logMsgF("Dormant... ID = %llu", MSG_NORMAL, (*it).first);
				uint64 waitTime = NpcManager::Instance()->handleNpc(npc, callTime - (*it).second);
				
				if (waitTime)
				{
					// Set next execution time. 
					(*it).second = callTime + waitTime;
				}
				else
				{
					// gLogger->logMsgF("Removed dormant NPC handler... %llu", MSG_NORMAL, (*it).first);

					// Requested to remove the handler.
					it = mNpcDormantHandlers.erase(it);
				}
			}
			else
			{
				// Remove the expired object...
				it = mNpcDormantHandlers.erase(it);
				gLogger->logMsg("Removed expired dormant NPC handler...");
			}
		}
		else
		{
			++it;
		}
	}
	return true;
}

//======================================================================================================================
// 
//	Add a npc to the Ready queue.
// 

void WorldManager::addReadyNpc(uint64 creature, uint64 when)
{
	uint64 expireTime = Anh_Utils::Clock::getSingleton()->getLocalTime();

	mNpcReadyHandlers.insert(std::make_pair(creature, expireTime + when));
}

//======================================================================================================================
// 
//	Remove a npc from the Ready queue.
// 

void WorldManager::removeReadyNpc(uint64 creature)
{
	NpcReadyHandlers::iterator it = mNpcReadyHandlers.find(creature);
	if (it != mNpcReadyHandlers.end())
	{
		// Remove creature.
		mNpcReadyHandlers.erase(it);
	}
}

//======================================================================================================================
// 
//	Force a npc from the Ready queue to be handled at next tick.
// 

void WorldManager::forceHandlingOfReadyNpc(uint64 creature)
{
	NpcReadyHandlers::iterator it = mNpcReadyHandlers.find(creature);
	if (it != mNpcReadyHandlers.end())
	{
		// Change the event time to NOW.
		uint64 now = Anh_Utils::Clock::getSingleton()->getLocalTime();
		(*it).second = now;
	}
}

//======================================================================================================================
//
// Handle the queue of Ready npc's.
//

bool WorldManager::_handleReadyNpcs(uint64 callTime, void* ref)
{
	NpcReadyHandlers::iterator it = mNpcReadyHandlers.begin();
	while (it != mNpcReadyHandlers.end())
	{
		//  The timer has expired?
		if (callTime >= ((*it).second))
		{
			// Yes, handle it.
			NPCObject* npc = dynamic_cast<NPCObject*>(this->getObjectById((*it).first));
			if (npc)
			{
				// uint64 waitTime = NpcManager::Instance()->handleReadyNpc(creature, callTime - (*it).second);
				// gLogger->logMsg("Ready...");
				// gLogger->logMsgF("Ready... ID = %llu", MSG_NORMAL, (*it).first);
				uint64 waitTime = NpcManager::Instance()->handleNpc(npc, callTime - (*it).second);
				if (waitTime)
				{
					// Set next execution time. 
					(*it).second = callTime + waitTime;
				}
				else
				{
					// Requested to remove the handler.
					it = mNpcReadyHandlers.erase(it);
					// gLogger->logMsg("Removed ready NPC handler...");
				}
			}
			else
			{
				// Remove the expired object...
				it = mNpcReadyHandlers.erase(it);
				// gLogger->logMsg("Removed ready NPC handler...");
			}
		}
		else
		{
			++it;
		}
	}
	return true;
}

//======================================================================================================================
// 
//	Add a npc to the Active queue.
// 

void WorldManager::addActiveNpc(uint64 creature, uint64 when)
{
	uint64 expireTime = Anh_Utils::Clock::getSingleton()->getLocalTime();

	mNpcActiveHandlers.insert(std::make_pair(creature, expireTime + when));
}

//======================================================================================================================
// 
//	Remove a npc from the Active queue.
// 

void WorldManager::removeActiveNpc(uint64 creature)
{
	NpcActiveHandlers::iterator it = mNpcActiveHandlers.find(creature);
	if (it != mNpcActiveHandlers.end())
	{
		// Remove creature.
		mNpcActiveHandlers.erase(it);
	}
}

//======================================================================================================================
//
// Handle the queue of Active npc's.
//
bool WorldManager::_handleActiveNpcs(uint64 callTime, void* ref)
{
	NpcActiveHandlers::iterator it = mNpcActiveHandlers.begin();
	while (it != mNpcActiveHandlers.end())
	{
		//  The timer has expired?
		if (callTime >= ((*it).second))
		{
			// Yes, handle it.
			NPCObject* npc = dynamic_cast<NPCObject*>(this->getObjectById((*it).first));
			if (npc)
			{
				// uint64 waitTime = NpcManager::Instance()->handleActiveNpc(creature, callTime - (*it).second);
				// gLogger->logMsg("Active...");
				// gLogger->logMsgF("Active... ID = %llu", MSG_NORMAL, (*it).first);
				uint64 waitTime = NpcManager::Instance()->handleNpc(npc, callTime - (*it).second);
				if (waitTime)
				{
					// Set next execution time. 
					(*it).second = callTime + waitTime;
				}
				else
				{
					// Requested to remove the handler.
					it = mNpcActiveHandlers.erase(it);
					// gLogger->logMsg("Removed active NPC handler...");
				}
			}
			else
			{
				// Remove the expired object...
				it = mNpcActiveHandlers.erase(it);
				// gLogger->logMsg("Removed active NPC handler...");
			}
		}
		else
		{
			++it;
		}
	}
	return true;
}

//======================================================================================================================
// 
//	Get the spawn area for a region.
// 

const Anh_Math::Rectangle WorldManager::getSpawnArea(uint64 spawnRegionId)
{
	Anh_Math::Rectangle spawnArea(0,0,0,0);

	CreatureSpawnRegionMap::iterator it = mCreatureSpawnRegionMap.find(spawnRegionId);
	if (it != mCreatureSpawnRegionMap.end())
	{
		const CreatureSpawnRegion *creatureSpawnRegion = (*it).second;
		Anh_Math::Rectangle sa(creatureSpawnRegion->mPosX, creatureSpawnRegion->mPosZ, creatureSpawnRegion->mWidth ,creatureSpawnRegion->mLength);
		spawnArea = sa;
	}
	return spawnArea;
}
