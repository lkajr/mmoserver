/*
---------------------------------------------------------------------------------------
This source file is part of swgANH (Star Wars Galaxies - A New Hope - Server Emulator)
For more information, see http://www.swganh.org


Copyright (c) 2006 - 2008 The swgANH Team

---------------------------------------------------------------------------------------
*/

#ifndef ANH_ZONESERVER_NPC_OBJECT_H
#define ANH_ZONESERVER_NPC_OBJECT_H

#include "CreatureObject.h"
#include "NPC_Enums.h"


#define NPC_CHAT_SPAM_PROTECTION_TIME	10000

//=============================================================================

class ActiveConversation;
class ConversationPage;
class ConversationOption;
class PlayerObject;
class Conversation;

//=============================================================================

class DamageDealer;

typedef std::vector<DamageDealer*> DamageDealers;

class NPCObject : public CreatureObject
{
	public:

		friend class PersistentNPCFactory;
		friend class NonPersistentNpcFactory;

		typedef enum _Npc_AI_State
		{
			NpcIsDormant = 0,
			NpcIsReady,
			NpcIsActive,
		} Npc_AI_State;

		NPCObject();
		virtual ~NPCObject();

		// Spawn info.
		uint32			getNpcFamily() const { return mNpcFamily; }
		void			setNpcFamily(uint32 family){ mNpcFamily = family; }
		uint64			getTemplateId(void) const {return mNpcTemplateId;}
		void			setTemplateId(uint64 templateId) {mNpcTemplateId = templateId;}
		uint64			getRespawnDelay(void) const {return mRespawnDelay;}
		void			setRespawnDelay(uint64 respawnDelay) {mRespawnDelay = respawnDelay;}
		uint64			getCellIdForSpawn(void) const {return mCellIdForSpawn;}
		void			setCellIdForSpawn(uint64 cellIdForSpawn) {mCellIdForSpawn = cellIdForSpawn;}
		void			setSpawnPosition(Anh_Math::Vector3 spawnPosition) {mSpawnPosition = spawnPosition;}
		Anh_Math::Vector3 getSpawnPosition() const {return mSpawnPosition;}
		void			setSpawnDirection(Anh_Math::Quaternion spawnDirection) {mSpawnDirection = spawnDirection;}
		Anh_Math::Quaternion getSpawnDirection() const {return mSpawnDirection;}

	
		string			getTitle() const { return mTitle; }
		void			setTitle(string title){ mTitle = title; }

		virtual void	filterConversationOptions(ConversationPage* page,std::vector<ConversationOption*>* filteredOptions,PlayerObject* player){}
		virtual uint32	handleConversationEvent(ActiveConversation* av,ConversationPage* page,ConversationOption* option,PlayerObject* player){return 0;}
		virtual	bool	preProcessfilterConversation(ActiveConversation* av, Conversation* conversation, PlayerObject* player) {return false;}
		virtual void	postProcessfilterConversation(ActiveConversation* av,ConversationPage* page,PlayerObject* player) {}
		virtual void	prepareConversation(PlayerObject* player) {}
		virtual void	stopConversation(PlayerObject* player) {}
		void			restoreDefaultDirection() {mDirection = mDefaultDirection;}
		void			storeDefaultDirection() {mDefaultDirection = mDirection;}
		virtual void	restorePosition(PlayerObject* player) {}

		virtual void	handleEvents(void) { }
		virtual uint64	handleState(uint64 timeOverdue) {return 0; }

		virtual float	getMaxSpawnDistance(void) { return 0.0;}


		// Used for NPC movements
		void			setPositionOffset(Anh_Math::Vector3 positionOffset) {mPositionOffset = positionOffset;}
		Anh_Math::Vector3	getPositionOffset() const {return mPositionOffset;}

		Anh_Math::Vector3 getRandomPosition(Anh_Math::Vector3& currentPos, int32 offsetX, int32 offsetZ) const;
		float			getHeightAt2DPosition(float xPos, float zPos) const;
		void			setDirection(float deltaX, float deltaZ);
		
		void			moveAndUpdatePosition(void);
		void			updatePosition(uint64 parentId, Anh_Math::Vector3 newPosition);

		uint64			getLastConversationTarget()const { return mLastConversationTarget; }
		uint64			getLastConversationRequest() const { return mLastConversationRequest; }

		void			setLastConversationTarget(uint64 target){ mLastConversationTarget = target; }
		void			setLastConversationRequest(uint64 request){ mLastConversationRequest = request; }


		void			setRandomDirection(void);
		Npc_AI_State	getAiState(void) const;

		void	updateDamage(uint64 playerId, uint64 groupId, uint32 weaponGroup, int32 damage, uint8 attackerPosture, float attackerDistance);
		bool	attackerHaveAggro(uint64 attackerId);
		void	updateAggro(uint64 playerId, uint64 groupId, uint8 attackerPosture);
		void	updateAttackersXp(void);
		void	setBaseAggro(float baseAggro) {mBaseAggro = baseAggro; }
		bool	allowedToLoot(uint64 targetId, uint64 groupId);


	private:
		void	updateGroupDamage(DamageDealer* damageDealer);
		void	sendAttackersWeaponXp(PlayerObject* playerObject, uint32 weaponMask, int32 xp);
		void	updateAttackersWeaponAndCombatXp(uint64 playerId, uint64 groupId, int32 damageDone, int32 weaponUsedMask);
		int32	getWeaponXp(void) const {return mWeaponXp;}
	

	protected:
		float	getAttackRange(void) {return mAttackRange;}
		void	setAttackRange(float attackRange) {mAttackRange = attackRange;}
		void	setWeaponXp(int32 weaponXp) {mWeaponXp = weaponXp;}

		void			setAiState(Npc_AI_State state);
		
		uint32	mNpcFamily;
		uint64  mSpeciesId;
		string	mTitle; 

		uint64	mLastConversationTarget;
		uint64	mLastConversationRequest;
		

	private:
		// Spawn info.
		uint64	mNpcTemplateId;
		uint64	mLootGroupId;
		uint64  mCellIdForSpawn;
		Anh_Math::Quaternion	mSpawnDirection;
		Anh_Math::Vector3		mSpawnPosition;

		// Delay before the object will respawn. Period time taken from time of destruction from world (not the same as when you "die").
		uint64 mRespawnDelay;

		Npc_AI_State	mAiState;
		Anh_Math::Quaternion	mDefaultDirection;	// Default direction for npc-objects. Needed when players start turning the npc around.
		Anh_Math::Vector3		mPositionOffset;

		// Damage data

		// Default aggro, without any modifiers.
		float mBaseAggro;

		// Players that come within this range will (may) be attacked.
		float mAttackRange;

		// Weapon XP.
		int32	mWeaponXp;

		uint64	mLootAllowedById;

		DamageDealers	mDamageDealers;
		DamageDealers	mDamageByGroups;
};

//=============================================================================


class	NpcConversionTime
{
	public:
		NPCObject*	mNpc;
		uint64		mTargetId;		// This is used when we run instances, and to differ
		uint64		mGroupId;		// Player belonging to same group, will be handled as one "unit" regarding the timeer.
		uint64		mInterval;
};

#endif