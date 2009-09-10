/*
---------------------------------------------------------------------------------------
This source file is part of swgANH (Star Wars Galaxies - A New Hope - Server Emulator)
For more information, see http://www.swganh.org


Copyright (c) 2006 - 2009 The swgANH Team

---------------------------------------------------------------------------------------
*/

#ifndef ANH_ZONESERVER_WORLDMANAGER_H
#define ANH_ZONESERVER_WORLDMANAGER_H

#include "ObjectFactory.h"
#include "ZoneTree.h"
#include "ConversationManager.h"
#include "SkillManager.h"
#include "SchematicManager.h"
#include "ResourceManager.h"
#include "ResourceCollectionManager.h"
#include "DatabaseManager/DatabaseCallback.h"
#include "ScriptEngine/ScriptEventListener.h"
#include "ObjectFactoryCallback.h"
#include "Utils/Timer.h"
#include "Utils/TimerCallback.h"
//#include "Utils/mutex.h"
#include "MessageLib/MessageLib.h"
#include "ZoneServer.h"
#include "WorldManagerEnums.h"
#include "RegionObject.h"
#include "Weather.h"
#include "Utils/Scheduler.h"
#include "Utils/VariableTimeScheduler.h"
#include "NPCObject.h"
#include <boost/ptr_container/ptr_map.hpp>
#include <boost/pool/pool.hpp>
#include <vector>
#include "MissionObject.h"
#include "GroupObject.h"
#include "CharacterLoginHandler.h"


//#include "BuffManager.h"
//======================================================================================================================

#define	 gWorldManager	WorldManager::getSingletonPtr()

// forward declarations
class DispatchClient;
class Anh_Utils::Clock;
class WMAsyncContainer;
class Script;
class NPCObject;
class CreatureSpawnRegion;

// pwns all objects
typedef boost::ptr_map<uint64,Object>			ObjectMap;

// seperate map for qt regions, since ids may match object ids
typedef boost::ptr_map<uint32,QTRegion>			QTRegionMap;

// Maps for objects in world
typedef std::map<uint32,const PlayerObject*>	PlayerAccMap;
typedef std::map<uint64,RegionObject*>			RegionMap;
typedef std::vector<RegionObject*>				RegionDeleteList;

// Lists for objects in world
typedef std::vector<PlayerObject*>				PlayerList;
typedef std::vector<Shuttle*>					ShuttleList;
typedef std::vector<RegionObject*>				ActiveRegions;
typedef std::list<CreatureObject*>				CreatureQueue;
typedef std::vector<CraftingTool*>				CraftTools;
typedef std::vector<std::pair<uint64, NpcConversionTime*>>	NpcConversionTimers;
typedef std::map<uint64, uint64>				PlayerMovementUpdateMap;
typedef std::map<uint64, uint64>				CreatureObjectDeletionMap;
typedef std::map<uint64, uint64>				PlayerObjectReviveMap;

// Creature spawn regions.
typedef std::map<uint64, const CreatureSpawnRegion*>	CreatureSpawnRegionMap;

// Containers with handlers to Npc-objects handled by the NpcManager (or what we are going to call it its final version).
// The active container will be the most often checked, and the Dormant the less checked container.

// And yes. Handlers... handlers... no object refs that will be invalid all the time.
typedef std::map<uint64, uint64>				NpcDormantHandlers;
typedef std::map<uint64, uint64>				NpcReadyHandlers;
typedef std::map<uint64, uint64>				NpcActiveHandlers;

// AttributeKey map
typedef std::map<uint32,string>					AttributeKeyMap;
typedef std::map<uint32,uint32>					AttributeIDMap;

// non-persistent id set
typedef std::set<uint64>						NpIdSet;


//======================================================================================================================
//
// Container for asyncronous database queries
//
class WMAsyncContainer
{
	public:

		WMAsyncContainer(WMQuery query){ mQuery = query; mObject = NULL; mClient = NULL; mBool = false; }

		WMLogOut					mLogout;
		WMQuery						mQuery;
		Object*						mObject;
		DispatchClient*				mClient;
		bool						mBool;
		CharacterLoadingContainer*	clContainer;
};

//======================================================================================================================
//
// container for simple queryresults
//
class WMQueryContainer
{
	public:

		WMQueryContainer(){}

		uint64			mId;
		string			mString;
};

//======================================================================================================================
//
// WorldManager
//
class WorldManager : public ObjectFactoryCallback, public DatabaseCallback, public TimerCallback
{
	public:

		static WorldManager*	getSingletonPtr() { return mSingleton; }
		static WorldManager*	Init(uint32 zoneId, ZoneServer* zoneServer,Database* database);
		void					Shutdown();

		void					Process();
		
		uint32					getZoneId() { return mZoneId; }
		WMState					getState(){ return mState; }
		uint64					getServerTime(){ return mServerTime; }
		Database*				getDatabase(){ return mDatabase; }

		// DatabaseCallback
		virtual void			handleDatabaseJobComplete(void* ref,DatabaseResult* result);

		// ObjectFactoryCallback
		virtual void			handleObjectReady(Object* object,DispatchClient* client);

		// TimerCallback
		virtual void			handleTimer(uint32 id, void* container);

		// add / delete an object, make sure to cleanup any other references
		bool					existObject(Object* object);	// Returns true if object does exist.
		void					addObject(Object* object,bool manual = false);
		void					destroyObject(Object* object);
		Object*					getObjectById(uint64 objId);

		// Find object owned by "player"
		uint64					getObjectOwnedBy(uint64 theOwner);

		// spatial query, creates all objects in range for each other
		void					initObjectsInRange(PlayerObject* playerObject);
		void					initPlayersInRange(Object* object,PlayerObject* player);

		// adds a creatures commandqueue to the main process queue
		uint64					addObjControllerToProcess(ObjectController* objController){ return((mObjControllerScheduler->addTask(fastdelegate::MakeDelegate(objController,&ObjectController::process),1,0,NULL))); }
		void					removeObjControllerToProcess(uint64 taskId);

		// adds a creatures ham which needs regeneration
		uint64					addCreatureHamToProccess(Ham* ham){ return((mHamRegenScheduler->addTask(fastdelegate::MakeDelegate(ham,&Ham::regenerate),1,1000,NULL))); }
		void					removeCreatureHamToProcess(uint64 taskId);
		bool					checkTask(uint64 id){return mHamRegenScheduler->checkTask(id);}

		// adds a mission that needs checking
		uint64					addMissionToProcess(MissionObject* mission){ return mMissionScheduler->addTask(fastdelegate::MakeDelegate(mission,&MissionObject::check),1,10000,NULL); }
		void					removeMissionFromProcess(uint64 taskId) { mMissionScheduler->removeTask(taskId); }
		bool					checkForMissionProcess(uint64 taskId) { return mMissionScheduler->checkTask(taskId); }

		// adds an performing entertainer which heals/gets exp
		uint64					addEntertainerToProccess(CreatureObject* entertainerObject,uint32 tick){ return((mEntertainerScheduler->addTask(fastdelegate::MakeDelegate(entertainerObject,&CreatureObject::handlePerformanceTick),1,tick,NULL))); }
		void					removeEntertainerToProcess(uint64 taskId);

		// adds a Buff which Ticks
		uint64					addBuffToProcess(Buff* buff);
		void					removeBuffToProcess(uint64 taskId);

		// saves a player asyncronously to the database
		void					savePlayer(uint32 accId,bool remove, WMLogOut mLogout, CharacterLoadingContainer* clContainer = NULL);

		// saves a player synched to the database
		void					savePlayerSync(uint32 accId,bool remove);

		// find a player, returns NULL if not found
		PlayerObject*			getPlayerByAccId(uint32 accId);

		// adds a player to the timeout queue, will save and remove him, when timeout occurs
		void					addDisconnectedPlayer(PlayerObject* playerObject);

		// removes player from the timeout list and adds him to the world
		void					addReconnectedPlayer(PlayerObject* playerObject);

		// adds dead creature object to the pool of objects with delayed destruction.
		void					addCreatureObjectForTimedDeletion(uint64 creatureId, uint64 when);
		
		// adds dead object to the pool of objects to be send to nearest cloning facility.
		void					addPlayerObjectForTimedCloning(uint64 playerId, uint64 when);
		
		// remove dead player object from the pool of objects to be send to nearest cloning facility.
		void					removePlayerObjectForTimedCloning(uint64 playerId);

		// removes player from the timeout list
		void					removePlayerFromDisconnectedList(PlayerObject* playerObject);

		// adds a shuttle
		void					addShuttle(Shuttle* shuttle){ mShuttleList.push_back(shuttle); }

		// add / remove busy crafting tools
		void					addBusyCraftTool(CraftingTool* tool);
		void					removeBusyCraftTool(CraftingTool* tool);

		// add / remove expired npc conversations.
		void					addNpcConversation(uint64 interval, NPCObject* npc);

		// add player update of known objects and position in world. 
		void					addPlayerMovementUpdateTime(PlayerObject* player, uint64 when);


		// check if objects are in range. Handles cell and buildings, but not distance between buildings or buildings and outside.
		bool					objectsInRange(uint64 obj1Id, uint64 obj2Id, float range);
		bool					objectsInRange(Anh_Math::Vector3 obj1Position,  uint64 obj1ParentId, uint64 obj2Id, float range);

		// Add-remove npc from Npc-handler queue's.
		void					addDormantNpc(uint64 creature, uint64 when);
		void					removeDormantNpc(uint64 creature);
		void					forceHandlingOfDormantNpc(uint64 creature);
		
		void					addReadyNpc(uint64 creature, uint64 when);
		void					removeReadyNpc(uint64 creature);
		void					forceHandlingOfReadyNpc(uint64 creature);

		void					addActiveNpc(uint64 creature, uint64 when);
		void					removeActiveNpc(uint64 creature);


		const					Anh_Math::Rectangle getSpawnArea(uint64 spawnRegionId);

		// retrieve object maps
		ObjectMap*				getWorldObjectMap(){ return &mObjectMap; }
		const PlayerAccMap*		getPlayerAccMap(){ return &mPlayerAccMap; }
		ShuttleList*			getShuttleList(){ return &mShuttleList; }

		// retrieve spatial index for this zone
		ZoneTree*				getSI(){ return mSpatialIndex; }

		// removes player from the current scene, and starts a new one after updating his position
		void					warpPlanet(PlayerObject* playerObject,Anh_Math::Vector3 destination,uint64 parentId,Anh_Math::Quaternion direction = Anh_Math::Quaternion());

		// get a client effect string by its id
		string					getClientEffect(uint32 effectId){ return mvClientEffects[effectId - 1]; }

		// get sound string by its id
		string					getSound(uint32 soundId){ return mvSounds[soundId - 1]; }
		// get a mood string by its id
		string					getMood(uint32 moodId){ return mvMoods[moodId]; }
		// get an attribute key
		string					getAttributeKey(uint32 keyId);
		// get an attribute ID		
		uint64					getAttributeId(uint32 keyId);
		// get a npc animation
		string					getNpcConverseAnimation(uint32 animId){ return mvNpcConverseAnimations[animId - 1]; }
		// get a random chat phrase
		std::pair<string,uint32>	getNpcChatter(uint32 id){ return mvNpcChatter[id]; }
		std::pair<string,uint32>	getRandNpcChatter();

		// get planet, trn file name
		int8*					getPlanetNameThis(){ return mvPlanetNames[mZoneId].getAnsi(); }
		int8*					getPlanetNameById(uint8 planetId){ return mvPlanetNames[planetId].getAnsi(); }
		int32					getPlanetIdByName(string name);

		int8*					getTrnFileThis(){ return mvTrnFileNames[mZoneId].getAnsi(); }
		int8*					getTrnFileById(uint8 trnId){ return mvTrnFileNames[trnId].getAnsi(); }

		// get total count of planets
		uint32					getPlanetCount(){ return mvPlanetNames.size(); }

		// region methods
		void					addRemoveRegion(RegionObject* region){mRegionDeleteList.push_back(region);}				//we store here regions that are due to be deleted after every iteration through active regions the list is iterated and its contents removed and destroyed
		RegionObject*			getRegionById(uint64 regionId);
		void					addActiveRegion(RegionObject* regionObject){ mActiveRegions.push_back(regionObject); }
		void					removeActiveRegion(RegionObject* regionObject);
		QTRegion*				getQTRegion(uint32 id);
		QTRegionMap*			getQTRegionMap(){ return &mQTRegionMap; }
		RegionMap*				getRegionMap(){ return &mRegionMap; }

		Anh_Utils::Scheduler*	getPlayerScheduler(){ return mPlayerScheduler; }

		Weather*				getCurrentWeather(){ return &mCurrentWeather; }
		void					updateWeather(float cloudX,float cloudY,float cloudZ,uint32 weatherType);
		void					zoneSystemMessage(std::string message);

		void					ScriptRegisterEvent(void* script,std::string eventFunction);

		// non-persistent ids in use
		uint64					getRandomNpId();
		bool					removeNpId(uint64 id);
		bool					checkdNpId(uint64 id);
		uint64					getRandomNpNpcIdSequence();

		//get the current tick
		uint64					GetCurrentGlobalTick();

		//load the tick from db
		void					LoadCurrentGlobalTick();

		bool					_handleTick(uint64 callTime,void* ref);

		~WorldManager();

		AttributeKeyMap				mObjectAttributeKeyMap;

	private:

		WorldManager(uint32 zoneId, ZoneServer* zoneServer,Database* database);

		// load the global ObjectControllerCommandMap, maps command crcs to ObjController function pointers
		void	_loadObjControllerCommandMap();

		// initializations after completed object load
		void	_handleLoadComplete();

		bool					addNpId(uint64 id);

		// timed subsystems 
		bool	_handleServerTimeUpdate(uint64 callTime,void* ref);
		bool	_handleShuttleUpdate(uint64 callTime,void* ref);
		bool	_handleDisconnectUpdate(uint64 callTime,void* ref);
		bool	_handleRegionUpdate(uint64 callTime,void* ref);
		bool	_handleCraftToolTimers(uint64 callTime,void* ref);
		bool	_handleNpcConversionTimers(uint64 callTime,void* ref);
		bool	_handleFireworkLaunchTimers(uint64 callTime,void* ref);

		bool	_handlePlayerMovementUpdateTimers(uint64 callTime, void* ref);
		void	removePlayerMovementUpdateTime(PlayerObject* player);

		bool	_handleGeneralObjectTimers(uint64 callTime, void* ref);
		bool	_handleGroupObjectTimers(uint64 callTime, void* ref);

		bool	_handleDormantNpcs(uint64 callTime, void* ref);
		bool	_handleReadyNpcs(uint64 callTime, void* ref);
		bool	_handleActiveNpcs(uint64 callTime, void* ref);
				
		void	_startWorldScripts();

		// process schedulers
		void	_processSchedulers();

		// load buildings and their contents
		void	_loadBuildings();

		// loads all child objects of the given parent
		void	_loadAllObjects(uint64 parentId);

		// load our script hooks
		void	_registerScriptHooks();

		static WorldManager*		mSingleton;
		static bool					mInsFlag;

		Database*					mDatabase;
		ZoneTree*					mSpatialIndex;
		Anh_Utils::Scheduler*		mNpcManagerScheduler;
		Anh_Utils::Scheduler*		mSubsystemScheduler;
		Anh_Utils::Scheduler*		mObjControllerScheduler;
		Anh_Utils::Scheduler*		mHamRegenScheduler;
		Anh_Utils::Scheduler*		mMissionScheduler;
		Anh_Utils::Scheduler*		mPlayerScheduler;
		Anh_Utils::Scheduler*		mEntertainerScheduler;
		Anh_Utils::VariableTimeScheduler* mBuffScheduler;

		ActiveRegions				mActiveRegions;
		QTRegionMap					mQTRegionMap;
		RegionMap					mRegionMap;
		RegionDeleteList			mRegionDeleteList;
		ObjectMap					mObjectMap;
		PlayerAccMap				mPlayerAccMap;
		ShuttleList					mShuttleList;
		CraftTools					mBusyCraftTools;
		NpcConversionTimers			mNpcConversionTimers;
		PlayerMovementUpdateMap		mPlayerMovementUpdateMap;
		CreatureObjectDeletionMap	mCreatureObjectDeletionMap;
		PlayerObjectReviveMap		mPlayerObjectReviveMap;

		NpcDormantHandlers			mNpcDormantHandlers;
		NpcReadyHandlers			mNpcReadyHandlers;
		NpcActiveHandlers			mNpcActiveHandlers;

		NpIdSet						mUsedTmpIds;

		CreatureSpawnRegionMap		mCreatureSpawnRegionMap;
		uint32						mZoneId;
		WMState						mState;

		ZoneServer*					mZoneServer;

		uint32						mTotalObjectCount;

		PlayerList					mPlayersToRemove;

		CreatureQueue				mObjControllersToProcess;
		uint64						mObjControllersProcessTimeLimit;

		uint64						mTick;
		uint64						mServerTime;
		Weather						mCurrentWeather;
		
		BStringVector				mvClientEffects;
		BStringVector				mvPlanetNames;
		BStringVector				mvTrnFileNames;
		BStringVector				mvMoods;
		BStringVector				mvSounds;
		BStringVector				mvNpcConverseAnimations;

		std::vector<std::pair<string,uint32> >	mvNpcChatter;

		ScriptList					mWorldScripts;
		ScriptEventListener			mWorldScriptsListener;
		
		boost::pool<boost::default_user_allocator_malloc_free>	mWM_DB_AsyncPool;
		// ZThread::RecursiveMutex		mSessionMutex;

};

//======================================================================================================================


#endif // ANH_ZONESERVER_WORLDMANAGER_H


