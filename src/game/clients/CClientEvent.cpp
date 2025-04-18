#include "../../common/resource/CResourceLock.h"
#include "../../common/CException.h"
#include "../../common/CExpression.h"
#include "../../network/CClientIterator.h"
#include "../../network/receive.h"
#include "../../network/send.h"
#include "../chars/CChar.h"
#include "../chars/CCharNPC.h"
#include "../items/CItemContainer.h"
#include "../items/CItemMessage.h"
#include "../items/CItemMulti.h"
#include "../items/CItemVendable.h"
#include "../uo_files/uofiles_enums_creid.h"
#include "../CSector.h"
#include "../CServer.h"
#include "../CWorld.h"
#include "../CWorldGameTime.h"
#include "../CWorldMap.h"
#include "../CWorldSearch.h"
#include "../triggers.h"
#include "CClient.h"


/////////////////////////////////
// Events from the Client.

lpctstr const CClient::sm_szCmd_Redirect[13] =
{
	"BANK",
	"CONTROL",
	"DESTROY",
	"DUPE",
	"FORGIVE",
	"JAIL",
	"KICK",
	"KILL",
	"NUDGEDOWN",
	"NUDGEUP",
	"PRIVSET",
	"REMOVE",
	"SHRINK",
};

void CClient::Event_ChatButton(const nachar* pszName) // Client's chat button was pressed
{
	ADDTOCALLSTACK("CClient::Event_ChatButton");
	// See if they've made a chatname yet
	// m_ChatPersona.SetClient(this);

	if (m_pChar == nullptr)
		return;
	m_fUseNewChatSystem = (GetNetState()->isClientVersionNumber(MINCLIVER_NEWCHATSYSTEM) || GetNetState()->isClientVersionNumber(CLIENTTYPE_EC + MINCLIVER_NEWCHATSYSTEM_EC));

	if ( IsTrigUsed(TRIGGER_USERCHATBUTTON) )
	{
		if (m_pChar && m_pChar->OnTrigger(CTRIG_UserChatButton, m_pChar) == TRIGRET_RET_TRUE)
			return;
	}
	GetChar()->SetTriggerActive("UserChatButton");	// dirty fix for SA Classic clients with injection moving a lot when using chat button, we set 'active trigger' to this, so we check it back on the packet to limit the amount of steps to do.

	ASSERT(GetAccount());

	if (!m_fUseNewChatSystem && GetAccount()->m_sChatName.IsEmpty())
	{
		if ((pszName == nullptr) || (pszName[0] == 0))
		{
			addChatSystemMessage(CHATCMD_SetChatName);
			return;
		}

		tchar szChatName[ MAX_NAME_SIZE * 2 + 2 ];
		CvtNETUTF16ToSystem( szChatName, sizeof(szChatName), pszName, 128 );

		if (!CChat::IsValidName(szChatName, true) || g_Accounts.Account_FindChat(szChatName)) // Check for legal name, duplicates, etc
		{
			addChatSystemMessage(CHATCMD_SetChatName);
			return;
		}
		GetAccount()->m_sChatName = szChatName;
	}

	// Ok, below here we have a chat system nickname
	// Tell the chat system it has a new client using it
	addChatWindow();
	GetChar()->SetTriggerActive();
}

void CClient::Event_ChatText( const nachar* pszText, int len, CLanguageID lang ) // Text from a client
{
	ADDTOCALLSTACK("CClient::Event_ChatText");
	// Just send it all to the chat system
	g_Serv.m_Chats.Action( this, pszText, len, lang );
}

void CClient::Event_Item_Dye( CUID uid, HUE_TYPE wHue ) // Rehue an item
{
	ADDTOCALLSTACK("CClient::Event_Item_Dye");
	// CLIMODE_DYE : Result from addDyeOption()
	if (m_pChar == nullptr)
		return;

	CObjBase *pObj = uid.ObjFind();
	if ( !m_pChar->CanTouch(pObj) )
	{
		SysMessage(g_Cfg.GetDefaultMsg(DEFMSG_ITEMUSE_DYE_REACH));
		return;
	}
	if ( GetTargMode() != CLIMODE_DYE )
		return;

	ClearTargMode();

	if ( !IsPriv(PRIV_GM) )
	{
		if ( !pObj->IsChar() )
		{
			CItem *pItem = dynamic_cast<CItem *>(pObj);
			if (pItem == nullptr || (( pObj->GetIDCommon() != 0xFAB ) && (!pItem->IsType(IT_DYE_VAT) || !IsSetOF(OF_DyeType))))
				return;

			if ( wHue < HUE_BLUE_LOW )
				wHue = HUE_BLUE_LOW;
			if ( wHue > HUE_DYE_HIGH )
				wHue = HUE_DYE_HIGH;
		} else
			return;
	}
	else if ( pObj->IsChar() )
	{
		pObj->RemoveFromView();
		wHue |= HUE_UNDERWEAR;
	}

	pObj->SetHue(wHue, false, this->GetChar(), pObj);
	pObj->Update();
}


void CClient::Event_Tips(word i) // Tip of the day window
{
	ADDTOCALLSTACK("CClient::Event_Tips");
	if (i == 0)
		i = 1;
	CResourceLock s;
	if ( g_Cfg.ResourceLock( s, CResourceID( RES_TIP, (int)i )) == false )
	{
		// requested tip was not found, default to tip 1 if possible
		if ( i == 1 || ( g_Cfg.ResourceLock( s, CResourceID( RES_TIP, 1 )) == false ))
			return;

		i = 1;
	}

	addScrollScript( s, SCROLL_TYPE_TIPS, i + 1 );
}

void CClient::Event_Book_Title( CUID uid, lpctstr pszTitle, lpctstr pszAuthor )
{
	ADDTOCALLSTACK("CClient::Event_Book_Title");
	// XCMD_BookOpen : user is changing the books title/author info.
	if ( m_pChar == nullptr )
		return;

	CItemMessage * pBook = dynamic_cast <CItemMessage *> (uid.ItemFind());
	if ( !m_pChar->CanTouch(pBook) )
	{
		SysMessage( g_Cfg.GetDefaultMsg(DEFMSG_REACH_FAIL) );
		return;
	}
	if ( !pBook->IsBookWritable() )
		return;

	if ( Str_Check(pszTitle) || Str_Check(pszAuthor) )
		return;

	pBook->SetName(pszTitle);
	pBook->m_sAuthor = pszAuthor;
}

void CClient::Event_Item_Pickup(CUID uid, word amount) // Client grabs an item
{
	ADDTOCALLSTACK("CClient::Event_Item_Pickup");
	EXC_TRY("CClient::Event_Item_Pickup");
	// Player/client is picking up an item.
	if ( m_pChar == nullptr )
		return;

	EXC_SET_BLOCK("Item");
	CItem	*pItem = uid.ItemFind();
	if ( !pItem || pItem->IsWeird() )
	{
		EXC_SET_BLOCK("Item - addObjectRemove(uid)");
		addObjectRemove(uid);
		EXC_SET_BLOCK("Item - addItemDragCancel(0)");
		new PacketDragCancel(this, PacketDragCancel::CannotLift);
		return;
	}

	EXC_SET_BLOCK("FastLoot");
	//	fastloot (,emptycontainer) protection
	const int64 iCurTime = CSTime::GetMonotonicSysTimeMilli();
	if ( m_tNextPickup > iCurTime)
	{
		EXC_SET_BLOCK("FastLoot - addItemDragCancel(0)");
		new PacketDragCancel(this, PacketDragCancel::CannotLift);
		return;
	}
	m_tNextPickup = iCurTime + (MSECS_PER_SEC/3);    // Using time in MSECS to work with this packet.

	EXC_SET_BLOCK("Origin");
	// Where is the item coming from ? (just in case we have to toss it back)
	CObjBase * pObjParent = dynamic_cast <CObjBase *>(pItem->GetParent());
	m_Targ_Prv_UID = pObjParent ? pObjParent->GetUID() : CUID();
	m_Targ_p = pItem->GetUnkPoint();

	EXC_SET_BLOCK("ItemPickup");
	const int tempamount = m_pChar->ItemPickup(pItem, amount);
	if ( tempamount < 0 )
	{
		EXC_SET_BLOCK("ItemPickup - addItemDragCancel(0)");
        if (pItem->GetType() == IT_CORPSE)
        {
            // You shouldn't even be able to pick it up if you aren't a GM, but some 7.x client versions do send nonetheless a pickup
            //  request packet: in this case, we have to prevent it from picking up the corpse.
            new PacketDragCancel(this, PacketDragCancel::Other);

            // This Update() fixes a client-side bug: if a char with GM on sees a new corpse, then turns GM off and tries to drag it, the dragging is cancelled but
            //  the corpse will take the appearance of an ogre until a new Update()
            pItem->Update();
        }
        else
            new PacketDragCancel(this, PacketDragCancel::CannotLift);
		return;
	}
	else if ( tempamount > 1 )
		m_tNextPickup += MSECS_PER_TENTH;	// +100 msec if amount should slow down the client

	SOUND_TYPE iSnd = (SOUND_TYPE)(pItem->GetDefNum("PICKUPSOUND", true));
	addSound(iSnd ? iSnd : (SOUND_TYPE)SOUND_USE_CLOTH);

	EXC_SET_BLOCK("TargMode");
	SetTargMode(CLIMODE_DRAG);
	m_Targ_UID = uid;
	EXC_CATCH;
}

void CClient::Event_Item_Drop_Fail( CItem *pItem )
{
	ADDTOCALLSTACK("CClient::Event_Item_Drop_Fail");
	// The item was in the LAYER_DRAGGING.
	// Try to bounce it back to where it came from.
	if ( !pItem || (pItem != m_pChar->LayerFind(LAYER_DRAGGING)) )
		return;

	CItemContainer *pPrevCont = static_cast<CItemContainer *>(m_Targ_Prv_UID.ItemFind());
	if ( pPrevCont )
	{
		pPrevCont->ContentAdd(pItem, m_Targ_p);
		return;
	}

	CChar *pPrevChar = m_Targ_Prv_UID.CharFind();
	if ( pPrevChar )
	{
		pPrevChar->ItemEquip(pItem);
		return;
	}

	pItem->MoveToCheck(m_pChar->GetTopPoint()); // Drop the item at player foot
}

void CClient::Event_Item_Drop( CUID uidItem, CPointMap pt, CUID uidOn, uchar gridIndex )
{
	ADDTOCALLSTACK("CClient::Event_Item_Drop");
	// This started from the Event_Item_Pickup()
	if ( !m_pChar )
		return;

	CItem * pItem = uidItem.ItemFind();
	CObjBase * pObjOn = uidOn.ObjFind();

	// Are we out of sync ?
	if ( pItem == nullptr ||
		pItem == pObjOn ||	// silliness.
		GetTargMode() != CLIMODE_DRAG ||
		pItem != m_pChar->LayerFind( LAYER_DRAGGING ))
	{
		new PacketDragCancel(this, PacketDragCancel::Other);
		return;
	}

	ClearTargMode();	// done dragging

	if (pItem->IsAttr(ATTR_QUESTITEM))
	{
		// These items can be dropped only on player backpack or trash can
		CItem *pPack = dynamic_cast<CItem *>(pObjOn);
		if (pPack && pPack->IsType(IT_TRASH_CAN))
		{
			addSound(pItem->GetDropSound(pObjOn));
			pItem->Delete();
			return;
		}
		else if ((pPack != m_pChar->LayerFind(LAYER_PACK)) && !IsPriv(PRIV_GM))
		{
			SysMessageDefault(DEFMSG_ITEM_CANTDROPTRADE);
			return Event_Item_Drop_Fail(pItem);
		}
	}

	if ( pObjOn != nullptr )	// Put on or in another object
	{
		if ( ! m_pChar->CanTouch( pObjOn ))	// Must also be LOS !
		{
			Event_Item_Drop_Fail( pItem );
			return;
		}

		if ( pObjOn->IsChar())	// Drop on a chars head.
		{
			CChar * pChar = dynamic_cast <CChar*>( pObjOn );
			if ( pChar != m_pChar )
			{
				if ( ! Cmd_SecureTrade( pChar, pItem ))
					Event_Item_Drop_Fail( pItem );
				return;
			}

			// dropped on myself. Get my Pack.
			pObjOn = m_pChar->GetPackSafe();
		}

		// On a container item ?
		CItemContainer * pContItem = dynamic_cast <CItemContainer *>( pObjOn );

		// Is the object on a person ? check the weight.
		CObjBaseTemplate * pObjTop = pObjOn->GetTopLevelObj();
		if ( pObjTop->IsChar())
		{
			CChar * pChar = dynamic_cast <CChar*>( pObjTop );
			ASSERT(pChar);
			if ( ! pChar->IsOwnedBy( m_pChar ) && !IsPriv(PRIV_GM))
			{
				// Slyly dropping item in someone elses pack.
				// or just dropping on their trade window.
				if ( ! Cmd_SecureTrade( pChar, pItem ))
					Event_Item_Drop_Fail( pItem );
				return;
			}
			if ( ! pChar->m_pPlayer )
			{
				pItem->ClrAttr(ATTR_OWNED);

				// newbie items lose newbie status when transfered to NPC
				if ( !g_Cfg.m_fAllowNewbTransfer )
					pItem->ClrAttr(ATTR_NEWBIE);
			}
			if ( pChar->GetBank()->IsItemInside( pContItem ))
			{
				// Convert physical gold into virtual gold when drop it on bankbox
				if ( pItem->IsType(IT_GOLD) && (g_Cfg.m_iFeatureTOL & FEATURE_TOL_VIRTUALGOLD) )
				{
					pChar->m_virtualGold += pItem->GetAmount();
					SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_BVBOX_DEPOSITED), pItem->GetAmount());
					addSound(pItem->GetDropSound(pObjOn));
					pItem->Delete();
					return;
				}

				// Diff Weight restrict for bank box and items in the bank box.
				if ( ! pChar->GetBank()->CanContainerHold( pItem, m_pChar ))
				{
					Event_Item_Drop_Fail( pItem );
					return;
				}
			}
		}

		if (pObjTop->IsItem())
		{
			CItemContainer * pTopContainer = dynamic_cast<CItemContainer*>(pObjTop);
			if (pTopContainer && !pTopContainer->CanContainerHold(pItem, m_pChar))
			{
				Event_Item_Drop_Fail(pItem);
				return;
			}
		}
        else // pObjTop may not be an item, it may be a character (eg: drop in the backpack or the bankbox)
        {
            if (pObjOn->IsItem() && pObjOn->IsContainer())
            {
                CItemContainer* pAboveContainer = static_cast<CItemContainer*>(pObjOn);
                while (pAboveContainer) // do a recursive check.
                {
                    if (!pAboveContainer->CanContainerHold(pItem, m_pChar))
                    {
                        Event_Item_Drop_Fail(pItem);
                        return;
                    }
                    CItemContainer* pNextContainer = static_cast<CItemContainer*>(static_cast<CItem*>(pObjOn)->GetTopContainer());
                    pAboveContainer = pNextContainer == pAboveContainer ? nullptr : pNextContainer;
                }
            }
        }

		if ( pContItem != nullptr )
		{
			//	bug with shifting selling list by gold coins
			if ( pContItem->IsType(IT_EQ_VENDOR_BOX) &&
				( pItem->IsType(IT_GOLD) || pItem->IsType(IT_COIN) ))
			{
				Event_Item_Drop_Fail( pItem );
				return;
			}
		}

		CObjBase *pOldCont = pItem->GetContainer();
		if (( IsTrigUsed(TRIGGER_DROPON_ITEM) ) || ( IsTrigUsed(TRIGGER_ITEMDROPON_ITEM) ))
		{
			CScriptTriggerArgs Args( pObjOn );
			if ( pItem->OnTrigger( ITRIG_DROPON_ITEM, m_pChar, &Args ) == TRIGRET_RET_TRUE )
			{
				Event_Item_Drop_Fail( pItem );
				return;
			}
		}

		if ( pOldCont != pItem->GetContainer() )
			return;

		CItem * pItemOn = dynamic_cast <CItem*> ( pObjOn );
		if (( pItemOn ) && (( IsTrigUsed(TRIGGER_DROPON_SELF) ) || ( IsTrigUsed(TRIGGER_ITEMDROPON_SELF) )))
		{
            CItem* pPrevCont = dynamic_cast<CItem*>(pItem->GetContainer());
			CScriptTriggerArgs Args( pItem );
			if ( pItemOn->OnTrigger( ITRIG_DROPON_SELF, m_pChar, &Args ) == TRIGRET_RET_TRUE )
			{
                CItem* pCont = dynamic_cast<CItem*>(pItem->GetContainer());
                if (pPrevCont == pCont)
				    Event_Item_Drop_Fail( pItem );
				return;
			}
		}

		if ( pContItem != nullptr )
		{
			const bool isBank = pContItem->IsType( IT_EQ_BANK_BOX );
			bool isCheating = false;
			if (isBank)
			{
				isCheating = pContItem->m_itEqBankBox.m_pntOpen != m_pChar->GetTopPoint();
			}
			else
			{
				const CItemContainer* pBank = m_pChar->GetBank();
                ASSERT(pBank);
				isCheating = pBank->IsItemInside(pContItem) && (pBank->m_itEqBankBox.m_pntOpen != m_pChar->GetTopPoint());
			}

			if ( isCheating )
			{
				Event_Item_Drop_Fail( pItem );
				return;
			}
			if ( !pContItem->CanContainerHold(pItem, m_pChar) )
			{
				Event_Item_Drop_Fail( pItem );
				return;
			}

			// only IT_GAME_PIECE can be dropped on IT_GAME_BOARD or clients will crash
			if (pContItem->IsType( IT_GAME_BOARD ) && !pItem->IsType( IT_GAME_PIECE ))
			{
				Event_Item_Drop_Fail( pItem );
				return;
			}

			// non-vendable items should never be dropped inside IT_EQ_VENDOR_BOX
			if ( pContItem->IsType( IT_EQ_VENDOR_BOX ) &&  !pItem->Item_GetDef()->GetMakeValue(0) )
			{
				SysMessageDefault( DEFMSG_MSG_ERR_NOT4SALE );
				Event_Item_Drop_Fail( pItem );
				return;
			}
		}
		else
		{
			// dropped on top of a non container item.
			// can i pile them ?
			// Still in same container.
			ASSERT(pItemOn);

			if (pItemOn->IsTypeMulti() && (GetNetState()->isClientVersionNumber(MINCLIVER_HS) || GetNetState()->isClientEnhanced()))
			{
				pt.m_x += pItemOn->GetTopPoint().m_x;
				pt.m_y += pItemOn->GetTopPoint().m_y;
				pt.m_z = pItemOn->GetTopPoint().m_z + pItemOn->GetHeight();
			}
			else
			{
				pObjOn = pItemOn->GetContainer();
				pt = pItemOn->GetUnkPoint();

				if ( ! pItem->Stack( pItemOn ))
				{
					if ( pItemOn->IsTypeSpellbook() )
					{
						if ( pItemOn->AddSpellbookScroll( pItem ))
						{
							SysMessage( g_Cfg.GetDefaultMsg( DEFMSG_CANT_ADD_SPELLBOOK ) );
							Event_Item_Drop_Fail( pItem );
							return;
						}
						// We only need to add a sound here if there is no
						// scroll left to bounce back.
						if (pItem->IsDeleted())
							addSound( 0x057, pItemOn );	// add to inv sound.
						Event_Item_Drop_Fail( pItem );
						return; // We can't drop any remaining scrolls
					}
					// Just drop on top of the current item.
					// Client probably doesn't allow this anyhow.
				}
			}
		}
	}
	else
	{
		if ( ! m_pChar->CanTouch( pt ))	// Must also be LOS !
		{
			Event_Item_Drop_Fail( pItem );
			return;
		}
	}

	// Game pieces can only be dropped on their game boards.
	if ( pItem->IsType(IT_GAME_PIECE))
	{
		if ( pObjOn == nullptr || m_Targ_Prv_UID != pObjOn->GetUID())
		{
			CItemContainer * pGame = dynamic_cast <CItemContainer *>( m_Targ_Prv_UID.ItemFind());
			if ( pGame != nullptr )
			{
				pGame->ContentAdd( pItem, m_Targ_p );
			}
			else
				pItem->Delete();	// Not sure what else to do with it.
			return;
		}
	}

	// do the dragging anim for everyone else to see.

	if ( pObjOn != nullptr )
	{
		// in pack or other CItemContainer.
		m_pChar->UpdateDrag( pItem, pObjOn );
		CItemContainer * pContOn = dynamic_cast <CItemContainer *>(pObjOn);

		if ( !pContOn )
		{
			if ( pObjOn->IsChar() )
			{
				CChar* pChar = dynamic_cast <CChar*>(pObjOn);

				if ( pChar )
					pContOn = pChar->GetBank( LAYER_PACK );
			}

			if ( !pContOn )
			{
				// on ground
				m_pChar->UpdateDrag( pItem, nullptr, &pt );
				m_pChar->ItemDrop( pItem, pt );
				return;
			}
		}

		ASSERT(pContOn);
		pContOn->ContentAdd( pItem, pt, gridIndex );
		addSound( pItem->GetDropSound( pObjOn ));
	}
	else
	{
		// on ground
		m_pChar->UpdateDrag( pItem, nullptr, &pt );
		m_pChar->ItemDrop( pItem, pt );
	}
}



void CClient::Event_Skill_Use( SKILL_TYPE skill ) // Skill is clicked on the skill list
{
	ADDTOCALLSTACK("CClient::Event_Skill_Use");
	// All the push button skills come through here.
	// Any "Last skill" macro comes here as well. (push button only)
	if ( m_pChar == nullptr )
		return;

	if ( !g_Cfg.m_SkillIndexDefs.valid_index(skill) )
	{
		SysMessage( "There is no such skill. Please tell support you saw this message.");
		return;
	}

	if ( !m_pChar->Skill_CanUse(skill) )
		return;

	if ( m_pChar->Skill_Wait(skill) )
		return;

	if ( IsTrigUsed(TRIGGER_SKILLSELECT) )
	{
		if ( m_pChar->Skill_OnCharTrigger( skill, CTRIG_SkillSelect ) == TRIGRET_RET_TRUE )
		{
			m_pChar->Skill_Fail( true );	// clean up current skill.
			return;
		}
	}

	if ( IsTrigUsed(TRIGGER_SELECT) )
	{
		if ( m_pChar->Skill_OnTrigger( skill, SKTRIG_SELECT ) == TRIGRET_RET_TRUE )
		{
			m_pChar->Skill_Fail( true );	// clean up current skill.
			return;
		}
	}

	SetTargMode();
	m_Targ_UID.InitUID();	// This is a start point for targ more.

	bool fCheckCrime	= false;
	bool fDoTargeting	= false;

	if ( g_Cfg.IsSkillFlag( skill, SKF_SCRIPTED ) )
	{
		const CSkillDef * pSkillDef = g_Cfg.GetSkillDef(skill);
		if (pSkillDef != nullptr && (pSkillDef->m_sTargetPrompt.IsEmpty() == false || pSkillDef->m_sTargetPromptCliloc.IsEmpty() == false))
		{
			m_tmSkillTarg.m_iSkill = skill;	// targetting what skill ?
			addTarget( CLIMODE_TARG_SKILL, pSkillDef->m_sTargetPrompt.GetBuffer(), false, fCheckCrime, 0, atoi(pSkillDef->m_sTargetPromptCliloc.GetBuffer()));
			return;
		}
		else
		{
			m_pChar->Skill_Start( skill );
		}
	}
	else switch ( skill )
	{
		case SKILL_ARMSLORE:
		case SKILL_ITEMID:
		case SKILL_ANATOMY:
		case SKILL_ANIMALLORE:
		case SKILL_EVALINT:
		case SKILL_FORENSICS:
		case SKILL_TASTEID:

		case SKILL_BEGGING:
		case SKILL_TAMING:
		case SKILL_REMOVETRAP:
			fCheckCrime = false;
			fDoTargeting = true;
			break;

		case SKILL_STEALING:
		case SKILL_ENTICEMENT:
		case SKILL_PROVOCATION:
		case SKILL_POISONING:
			// Go into targtting mode.
			fCheckCrime = true;
			fDoTargeting = true;
			break;

		case SKILL_STEALTH:	// How is this supposed to work.
		case SKILL_HIDING:
		case SKILL_SPIRITSPEAK:
		case SKILL_PEACEMAKING:
		case SKILL_DETECTINGHIDDEN:
		case SKILL_MEDITATION:
		case SKILL_IMBUING:
			// These start/stop automatically.
			m_pChar->Skill_Start(skill);
			return;

		case SKILL_TRACKING:
			Cmd_Skill_Tracking(UINT32_MAX, false);
			break;

		case SKILL_CARTOGRAPHY:
			// EMPTY. On OSI cartography is not used clicking on the skill button anymore.
			// This code is empty just for compatibility purposes if someone want customize
			// the softcoded skill to enable the button again using @Select trigger.
			break;

		case SKILL_INSCRIPTION:
			// Menu select for spell type.
			Cmd_Skill_Inscription();
			break;

		default:
			SysMessage( "There is no such skill. Please tell support you saw this message.");
			break;
	}

	if ( fDoTargeting )
	{
		// Go into targetting mode.
		const CSkillDef * pSkillDef = g_Cfg.GetSkillDef(skill);
		if (pSkillDef == nullptr || (pSkillDef->m_sTargetPrompt.IsEmpty() && pSkillDef->m_sTargetPromptCliloc.IsEmpty()))
		{
			DEBUG_ERR(( "%x: Event_Skill_Use bad skill %d\n", GetSocketID(), skill ));
			return;
		}

		m_tmSkillTarg.m_iSkill = skill;	// targetting what skill ?
		addTarget( CLIMODE_TARG_SKILL, pSkillDef->m_sTargetPrompt.GetBuffer(), false, fCheckCrime, 0 , atoi(pSkillDef->m_sTargetPromptCliloc.GetBuffer()));
		return;
	}
}



bool CClient::Event_CheckWalkBuffer(byte rawdir)
{
	ADDTOCALLSTACK("CClient::Event_CheckWalkBuffer");
	//Return False: block the step
	// Check if the client is trying to walk too fast.
	// Direction changes don't count.
	//NOTE: If WalkBuffer=20 in ini, it's egal 2000 here


	const int64 iCurTime = CSTime::GetMonotonicSysTimeMilli();
    int64 iTimeDiff = (int64)llabs(iCurTime - m_timeWalkStep);	// use absolute value to prevent overflows
	int64 iTimeMin = 0;  // minimum time to move 1 step in milliseconds
	m_timeWalkStep = iCurTime; //Take the time of step for the next time we enter here

	if (m_lastDir != rawdir) //Changing direction create some strange timer we only evaluate when going straight
	{
		m_lastDir = rawdir;
		return true;
	}


	// First step is to determine the theoric time(iTimeMin) to take the last step(s)
	/*		RUN /Walk
	Mount	100 / 200
	foot	200 / 400
	Speed Modes:
	0 = Foot=Normal, Mount=Normal
	1 = Foot=Double Speed, Mount=Normal
	2 = Foot=Always Walk, Mount=Always Walk (Half Speed)
	3 = Foot=Always Run, Mount=Always Walk
	4 = No Movement (handled by OnFreezeCheck)*/
	//Since we only check when we run, we don't care all walk situation

	if (m_pChar->IsStatFlag(STATF_ONHORSE | STATF_HOVERING)) //on horse or Gargoyle fly
		iTimeMin = 100;
	else //on foot
	{
		if (m_pChar->m_pPlayer && (m_pChar->m_pPlayer->m_speedMode == 1))
			iTimeMin = 100;
		else
			iTimeMin = 200;
	}

	if (!(iTimeDiff > iTimeMin + 350))
		// We don't want to do process if time is greater of 350 (Ping of player should be lower than this)
		// Accept a Big number cause a big offset on the average. When player stop moving, you'll always get big number.

	{
		if ( iTimeDiff > iTimeMin )
		// If the step time is greater than the theoric time there 4 reasons
		// It's the server process tick, player's ping, been a while since last step, change direction during run
		// Here we ajust TimeDiff using ini parameter
			//WalkRegen: Determine how the TimeDiff is ajust depending of the ping. Depending on setting, player will gain more point.
			//Default Value is 25
			//OVER default value: More permissive, more point earn, less false positive, more possibility to don't see high ping player
			//UNDER default value: Strick verification, more false positive (Not recommand to go under default value)
		{
			int64 iRegen = ((iTimeDiff - iTimeMin) * g_Cfg.m_iWalkRegen) / 20;

			// Get the ajust Timediff
			iTimeDiff = iTimeMin + iRegen;
		}

		// Create de average step value
		m_iWalkTimeAvg += iTimeDiff;
		m_iWalkTimeAvg -= iTimeMin;


		//WalkBuffer: Maximum buffer allow on player. Each good step give point what maximum point you want?
		//Ajust the maximum average to the define buffer
		if ( m_iWalkTimeAvg > g_Cfg.m_iWalkBuffer )
			m_iWalkTimeAvg = g_Cfg.m_iWalkBuffer;

		if ( IsPriv(PRIV_DETAIL) && IsPriv(PRIV_DEBUG) )
			SysMessagef("Walkcheck trace: timeDiff(%" PRId64 ") / timeMin(%" PRId64 "). curAvg(%lld)", iTimeDiff, iTimeMin, m_iWalkTimeAvg);

		// Checking if there a speehack
		if ( m_iWalkTimeAvg < 0 && iTimeDiff >= 0 )
		{
			// Walking too fast.
			m_iWalkTimeAvg = 500; //reset the average
			DEBUG_WARN(("%s (%x): Fast Walk ?\n", GetName(), GetSocketID()));
			if ( IsTrigUsed(TRIGGER_USEREXWALKLIMIT) )
			{
				if ( m_pChar->OnTrigger(CTRIG_UserExWalkLimit, m_pChar) != TRIGRET_RET_TRUE )
					return false;
			}
		}
	}
	return true;
}

bool CClient::Event_ExceededNetworkQuota(uchar uiType, int64 iBytes, int64 iQuota)
{
	ADDTOCALLSTACK("CClient::Event_ExceededNetworkQuota");

	CScriptTriggerArgs Args(uiType, iBytes, iQuota);
	Args.m_VarsLocal.SetStrNew("Account", GetName());
	Args.m_VarsLocal.SetStrNew("IP", GetPeer().GetAddrStr());

	TRIGRET_TYPE tRet = TRIGRET_RET_DEFAULT;
	g_Serv.r_Call("f_onclient_exceed_network_quota", this, &Args, nullptr, &tRet);

	if (tRet == TRIGRET_RET_FALSE)
	{
		return true;	// print log message
	}
	if (tRet == TRIGRET_RET_TRUE)
	{
		return false;	// no log message
	}

	addKick(&g_Serv, false);
	return true;
}


bool CClient::Event_Walk( byte rawdir, byte sequence ) // Player moves
{
	ADDTOCALLSTACK("CClient::Event_Walk");
	// The client is sending a walk request to server, so the server must check
	// if the movement is possible and reply with another allow/reject packet
	// Return:
	//  true    = walking was allowed
	//  false   = walking was rejected

	// The theory....
	// The client sometimes echos 1 or 2 zeros or invalid echos when you first start
	//	walking (the invalid non zeros happen when you log off and don't exit the
	//	client.exe all the way and then log back in, XXX doesn't clear the stack)

	if ( !m_pChar )
		return false;

	DIR_TYPE dir = DIR_TYPE(rawdir & 0xF);
	if ( dir >= DIR_QTY )
	{
		new PacketMovementRej(this, sequence);
		return false;
	}

	CPointMap pt = m_pChar->GetTopPoint();
	CPointMap ptOld = pt;

	if ( dir == m_pChar->m_dirFace )
	{
		// Move in this dir.
		pt.Move(dir);

		// Check the z height here.
		// The client already knows this but doesn't tell us.
		if ( m_pChar->CanMoveWalkTo(pt, true, false, dir) == nullptr )
		{
			new PacketMovementRej(this, sequence);
			return false;
		}

		// To get milliseconds precision we must get the system clock manually at each walk request (the server clock advances only at every tick).
		const int64 iCurTime = CWorldGameTime::GetCurrentTime().GetTimeRaw();

		if (!m_pChar->MoveToChar(pt, false, false))
		{
			new PacketMovementRej(this, sequence);
			return false;
		}

		// Check if I stepped on any item/teleport
		TRIGRET_TYPE iRet = m_pChar->CheckLocationEffects(false);
		if (iRet == TRIGRET_RET_FALSE)
		{
			m_pChar->SetUnkPoint(ptOld);	// we already moved, so move back to previous location
			new PacketMovementRej(this, sequence);
			return false;
		}

		// Set running flag if I'm running
		m_pChar->StatFlag_Mod(STATF_FLY, (rawdir & DIR_MASK_RUNNING) ? true : false);

		if (IsSetEF(EF_FastWalkPrevention) && !m_pChar->IsPriv(PRIV_GM))
		{
			// FIXME:THIS SYSTEM DO NOT WORK SEE DETAIL DOWN
			if (iCurTime < m_timeNextEventWalk)		// fastwalk detected (speedhack)
			{
				g_Log.Event(LOGL_WARN | LOGM_CHEAT, "Fastwalk detection for '%s', this player will notice a lag\n", GetAccount()->GetName());
				new PacketMovementRej(this, sequence);
				return false;
			}

			int64 iDelay = 0;
			if (m_pChar->IsStatFlag(STATF_ONHORSE | STATF_HOVERING) || (m_pChar->m_pPlayer->m_speedMode & 0x01))
				iDelay = (rawdir & DIR_MASK_RUNNING) ? 100 : 200;	// 100ms : 200ms
			else
				iDelay = (rawdir & DIR_MASK_RUNNING) ? 200 : 400;	// 200ms : 400ms

			iDelay -= 30; //Delay offset is set to be more permisif when player have lag or processor lack precision
			// This system do not work because the offset must be fine tune for each server and for EACH player and it's ping
			// For exemple, in local we set offset to 10 and there is no false-positive. If set offset to 30, Speedhack at 1.2 is not detect
			// On live server, with delay of 30, some player will experience false-positive some not. Player with good ping will be able to use speedhack without detection
			// FIXME: The offset delay should be calculate using the ping value of each player and a fix value of processor functionnality. The iDelay must ajust each tick depending of the ping
			// The buffer system Event_CheckWalkBuffer seem more acurate because it permit some ajustment.
			m_timeNextEventWalk = iCurTime + iDelay;
		}
		else if (m_pChar->IsStatFlag(STATF_FLY) && !m_pChar->IsPriv(PRIV_GM) && (g_Cfg.m_iWalkBuffer) && !m_pChar->GetRegion()->_pMultiLink && !Event_CheckWalkBuffer(rawdir) )
				//Run, Not GM , walkbuffer active on ini, not on multi (boat)
		{
			new PacketMovementRej(this, sequence);
			g_Log.Event(LOGL_WARN | LOGM_CHEAT, "PacketMovement Rejected for '%s', Speedhack or WalkRegen ini setting?\n", GetAccount()->GetName());
			m_timeLastEventWalk = iCurTime;
			++m_iWalkStepCount;					// Increase step count to use on next walk buffer checks
			return false;
		}

		// Are we invis ?
		m_pChar->CheckRevealOnMove();

		if ( iRet == TRIGRET_RET_TRUE )
		{
			new PacketMovementAck(this, sequence);
			m_pChar->UpdateMove(ptOld, this);	// Who now sees me ?
			addPlayerSee(ptOld);				// What new stuff do I now see ?

            if (m_pChar->m_pParty && ((m_iWalkStepCount % 10) == 0))	// Send map waypoint location to party members at each 10 steps taken (enhanced clients only)
            {
                m_pChar->m_pParty->UpdateWaypointAll(m_pChar, MAPWAYPOINT_PartyMember);
            }
		}

		m_timeLastEventWalk = iCurTime;
		++m_iWalkStepCount;					// Increase step count to use on next walk buffer checks
	}
	else
	{
		// Just a change in dir.
		new PacketMovementAck(this, sequence);
		m_pChar->m_dirFace = dir;
		m_pChar->UpdateMove(ptOld, this);	// Who now sees me ?
	}
	return true;
}

// Client selected an combat ability on book
void CClient::Event_CombatAbilitySelect(dword dwAbility)
{
    ADDTOCALLSTACK("CClient::Event_CombatAbilitySelect");
    if ( !m_pChar )
        return;

    if ( IsTrigUsed(TRIGGER_USERSPECIALMOVE) )
    {
        CScriptTriggerArgs Args;
        Args.m_iN1 = dwAbility;
        m_pChar->OnTrigger(CTRIG_UserSpecialMove, m_pChar, &Args);
    }
}

// Client selected an virtue on gump
void CClient::Event_VirtueSelect(dword dwVirtue, CChar *pCharTarg)
{
    ADDTOCALLSTACK("CClient::Event_VirtueSelect");
    if ( !m_pChar )
        return;

    if ( IsTrigUsed(TRIGGER_USERVIRTUE) )
    {
        CScriptTriggerArgs Args(pCharTarg);
        Args.m_iN1 = dwVirtue;
        m_pChar->OnTrigger(CTRIG_UserVirtue, m_pChar, &Args);
    }
}

void CClient::Event_CombatMode( bool fWar ) // Only for switching to combat mode
{
	ADDTOCALLSTACK("CClient::Event_CombatMode");
	// If peacemaking then this doens't work ??
	// Say "you are feeling too peacefull"
	if ( m_pChar == nullptr )
		return;

	bool fCleanSkill = true;

	if ( IsTrigUsed(TRIGGER_USERWARMODE) )
	{
		CScriptTriggerArgs Args;
		Args.m_iN1 = m_pChar->IsStatFlag(STATF_WAR) ? 1 : 0;
		Args.m_iN2 = 1;
		Args.m_iN3 = 0;
		if (m_pChar->OnTrigger(CTRIG_UserWarmode, m_pChar, &Args) == TRIGRET_RET_TRUE)
			return;

		if ( Args.m_iN2 == 0 )
			fCleanSkill = false;

		if ( Args.m_iN3 != 0 && Args.m_iN3 < 3)
			fWar = (Args.m_iN3 == 1 ? false : true);
	}

	m_pChar->StatFlag_Mod( STATF_WAR, fWar );

	if ( m_pChar->IsStatFlag( STATF_DEAD ))
		m_pChar->StatFlag_Mod( STATF_INSUBSTANTIAL, !fWar );	// manifest the ghost

	if ( fCleanSkill )
	{
		m_pChar->Skill_Fail( true );
		m_pChar->m_Fight_Targ_UID.InitUID();
		//DEBUG_WARN(("UserWarMode - Cleaning Skill Action\n"));
	}

	addPlayerWarMode();
    m_pChar->UpdateMode(m_pChar->IsStatFlag(STATF_DEAD), this);
}

bool CClient::Event_Command(lpctstr pszCommand, TALKMODE_TYPE mode)
{
	ADDTOCALLSTACK("CClient::Event_Command");
	if ( mode == TALKMODE_GUILD || mode == TALKMODE_ALLIANCE ) // guild and alliance don't pass this.
		return false;
	if ( pszCommand[0] == 0 )
		return true;		// should not be said
	if ( Str_Check(pszCommand) )
		return true;		// should not be said
	if ( ((m_pChar->GetDispID() == CREID_EQUIP_GM_ROBE) && (pszCommand[0] == '=')) //  WTF? In any case, keep using dispid, or it's bugged when you change character's dispid to c_man_gm.
        || (pszCommand[0] == g_Cfg.m_cCommandPrefix))
	{
		// Lazy :P
	}
	else
		return false;

	if ( !strnicmp(pszCommand, "q", 1) && ( GetPrivLevel() > PLEVEL_Player ) )
	{
		SysMessage("Probably you forgot about Ctrl?");
		return true;
	}

	bool fAllowCommand = true;
	bool fAllowSay = true;

	pszCommand += 1;
	GETNONWHITESPACE(pszCommand);
	fAllowCommand = g_Cfg.CanUsePrivVerb(this, pszCommand, this);

	if ( !fAllowCommand )
		fAllowSay = ( GetPrivLevel() <= PLEVEL_Player );

	//	filter on commands is active - so trigger it
	if ( !g_Cfg.m_sCommandTrigger.IsEmpty() )
	{
		CScriptTriggerArgs Args(pszCommand);
		Args.m_iN1 = fAllowCommand;
		Args.m_iN2 = fAllowSay;
		enum TRIGRET_TYPE tr;

		//	Call the filtering function
		if ( m_pChar->r_Call(g_Cfg.m_sCommandTrigger, m_pChar, &Args, nullptr, &tr) )
			if ( tr == TRIGRET_RET_TRUE )
				return (Args.m_iN2 != 0);

		fAllowCommand = ( Args.m_iN1 != 0 );
		fAllowSay = ( Args.m_iN2 != 0 );
	}

	if ( !fAllowCommand && !fAllowSay )
		SysMessage(g_Cfg.GetDefaultMsg(DEFMSG_MSG_ACC_PRIV));

	if ( fAllowCommand )
	{
		fAllowSay = false;

		// Assume you don't mean yourself !
		if ( FindTableHeadSorted( pszCommand, sm_szCmd_Redirect, ARRAY_COUNT(sm_szCmd_Redirect)) >= 0 )
		{
			// targetted verbs are logged once the target is selected.
			addTargetVerb(pszCommand, "");
		}
		else
		{
			CScript s(pszCommand);
			if ( !m_pChar->r_Verb(s, m_pChar) )
				SysMessageDefault(DEFMSG_CMD_INVALID);
		}
	}

	if ( GetPrivLevel() >= g_Cfg.m_iCommandLog )
		g_Log.Event(LOGM_GM_CMDS, "%x:'%s' commands '%s'=%d\n", GetSocketID(), GetName(), pszCommand, fAllowCommand);

	return !fAllowSay;
}

void CClient::Event_Attack( CUID uid )
{
	ADDTOCALLSTACK("CClient::Event_Attack");
	// d-click in war mode
	// I am attacking someone.
	if ( m_pChar == nullptr )
		return;

	CChar * pChar = uid.CharFind();
	if ( pChar == nullptr )
		return;

    bool fFail = pChar->Can(CAN_C_NONSELECTABLE);
    if (!fFail)
        fFail = !m_pChar->Fight_Attack(pChar);

	new PacketAttack(this, (fFail ? CUID() : pChar->GetUID()));
}

// Client/Player buying items from the Vendor

void CClient::Event_VendorBuy_Cheater( int iCode )
{
	ADDTOCALLSTACK("CClient::Event_VendorBuy_Cheater");

	// iCode descriptions
	static lpctstr constexpr sm_BuyPacketCheats[] =
	{
		"Other",
		"Bad vendor UID",
		"Bad item UID",
		"Requested items out of stock",
		"Total cost is too high"
	};

	g_Log.Event(LOGL_WARN|LOGM_CHEAT, "%x:Cheater '%s' is submitting illegal buy packet (%s)\n", GetSocketID(), GetAccount()->GetName(), sm_BuyPacketCheats[iCode]);
	SysMessage(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CANTBUY));
}

void CClient::Event_VendorBuy(CChar* pVendor, const VendorItem* items, uint uiItemCount)
{
    ADDTOCALLSTACK("CClient::Event_VendorBuy");
    if (m_pChar == nullptr || pVendor == nullptr || items == nullptr || uiItemCount <= 0)
        return;

    //We don't need to limit virtual golds to 32 bit int.
    const int64 kuiMaxCost = ((g_Cfg.m_iFeatureTOL & FEATURE_TOL_VIRTUALGOLD) ? (INT64_MAX / 2) : (INT32_MAX / 2));

    const bool fPlayerVendor = pVendor->IsStatFlag(STATF_PET);
    pVendor->GetBank(LAYER_VENDOR_STOCK);
    CItemContainer* pPack = m_pChar->GetPackSafe();

    CItemVendable* pItem;
    int64 iCostTotal = 0;

    //	Check if the vendor really has so much items
    for (uint i = 0; i < uiItemCount; ++i)
    {
        if ( items[i].m_serial.IsValidUID() == false )
            continue;

        pItem = dynamic_cast <CItemVendable *> (items[i].m_serial.ItemFind());
        if ( pItem == nullptr )
            continue;

        if ((items[i].m_vcAmount <= 0) || (items[i].m_vcAmount > pItem->GetAmount()))
        {
            pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CANTFULFILL));
            Event_VendorBuy_Cheater( 0x3 );
            return;
        }

        switch (pItem->GetType())
        {
            case IT_FIGURINE:
            {
                if (IsSetOF(OF_PetSlots))
                {
                    CItemBase* pItemPet = CItemBase::FindItemBase(pItem->GetID());
                    CCharBase* pPetDef = CCharBase::FindCharBase(CREID_TYPE(pItemPet->m_ttFigurine.m_idChar.GetResIndex()));
                    if (pPetDef)
                    {
                        const short uiFollowerSlots = n64_narrow_n16(pItem->GetDefNum("FOLLOWERSLOTS", true));
                        if (!m_pChar->FollowersUpdate(pVendor, (uiFollowerSlots * items[i].m_vcAmount), true))
                        {
                            m_pChar->SysMessageDefault(DEFMSG_PETSLOTS_TRY_CONTROL);
                            return;
                        }
                    }
                }
                break;
            }
            case IT_HAIR:
            {
                if (!m_pChar->IsPlayableCharacter())
                {
                    pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CANTBUY));
                    return;
                }
                break;
            }
            case IT_BEARD:
            {
                if ((m_pChar->GetDispID() != CREID_MAN) && (m_pChar->GetDispID() != CREID_GARGMAN) && !m_pChar->IsPriv(PRIV_GM))
                {
                    pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CANTBUY));
                    return;
                }
                break;
            }
        }

		iCostTotal += ((int64)(items[i].m_vcAmount) * items[i].m_price);
        if (iCostTotal > kuiMaxCost)
        {
            pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CANTFULFILL));
            Event_VendorBuy_Cheater( 0x4 );
            return;
        }
    }

	if (iCostTotal <= 0 )
	{
		pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_BUY_NOTHING));
		return;
	}
	iCostTotal = m_pChar->PayGold(pVendor, iCostTotal, nullptr, PAYGOLD_BUY);

    //	Check for gold being enough to buy this
    bool fBoss = pVendor->NPC_IsOwnedBy(m_pChar);
    if ( !fBoss )
    {
        if (g_Cfg.m_iFeatureTOL & FEATURE_TOL_VIRTUALGOLD)
        {
            if (m_pChar->m_virtualGold < iCostTotal)
            {
                pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_NOMONEY1));
                return;
            }
        }
        else
        {
            int iGold = m_pChar->GetPackSafe()->ContentConsumeTest(CResourceID(RES_TYPEDEF, IT_GOLD), (dword)iCostTotal);
            if (!g_Cfg.m_fPayFromPackOnly && iGold)
                iGold = m_pChar->ContentConsumeTest(CResourceID(RES_TYPEDEF, IT_GOLD), (int)iCostTotal);

            if (iGold)
            {
                pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_NOMONEY1));
                return;
            }
        }
    }

    //	Move the items bought into your pack.
	uint uiItemBlocked = 0;
    for (uint i = 0; i < uiItemCount; ++i)
    {
        if (items[i].m_serial.IsValidUID() == false)
            continue; //We need to continue for loop for other items not break.

        pItem = dynamic_cast <CItemVendable *> (items[i].m_serial.ItemFind());
        word amount = items[i].m_vcAmount;

        if (pItem == nullptr)
            continue;

        if ((IsTrigUsed(TRIGGER_BUY)) || (IsTrigUsed(TRIGGER_ITEMBUY)))
        {
			int64 iItemCost = int64(amount) * items[i].m_price;
            CScriptTriggerArgs Args( amount, iItemCost, pVendor );
            Args.m_VarsLocal.SetNum( "TOTALCOST", iCostTotal);
			if (pItem->OnTrigger(ITRIG_Buy, this->GetChar(), &Args) == TRIGRET_RET_TRUE)
			{
				iCostTotal -= iItemCost; //If we are blocking the transaction we should not pay for it!.
				uiItemBlocked++;
				continue;
			}
        }

        if (!fPlayerVendor) //NPC vendors
        {
            pItem->SetAmount(pItem->GetAmount() - amount);

            switch (pItem->GetType())
            {
                case IT_FIGURINE:
                {
                    for ( int f = 0; f < amount; ++f )
                        m_pChar->Use_Figurine(pItem);
                    goto do_consume;
                }
                case IT_BEARD:
                case IT_HAIR:
                {
                    //While we already checked beard and hair requirements we don't need any more checks for it.
                    CItem* pItemNew = CItem::CreateDupeItem(pItem);
                    m_pChar->LayerAdd(pItemNew); //Equip it, we don't need to drop it on backpack.
                    pItemNew->m_TagDefs.SetNum("NOSAVE", 0, true);
                    pItemNew->SetTimeoutS(55000); //Growing timer.
                    pVendor->UpdateAnimate(ANIM_ATTACK_1H_SLASH);
                    m_pChar->Sound(SOUND_SNIP);
                    break;
                }
                default:
                    break;
            }

            if ((amount > 1) && (!pItem->Item_GetDef()->IsStackableType()))
            {
                while (amount--)
                {
                    CItem * pItemNew = CItem::CreateDupeItem(pItem);
                    pItemNew->SetAmount(1);
                    pItemNew->m_TagDefs.SetNum("NOSAVE", 0, true);

                    if (!pPack->CanContainerHold(pItemNew, m_pChar) || (!m_pChar->CanCarry(pItemNew)))
                        m_pChar->ItemDrop( pItemNew, m_pChar->GetTopPoint() );
                    else
                        pPack->ContentAdd( pItemNew );
                }
            }
            else
            {
                CItem * pItemNew = CItem::CreateDupeItem(pItem);
                pItemNew->SetAmount(amount);
                pItemNew->m_TagDefs.SetNum("NOSAVE", 0, true);
                if (!pPack->CanContainerHold(pItemNew, m_pChar) || (!m_pChar->CanCarry(pItemNew)))
                    m_pChar->ItemDrop(pItemNew, m_pChar->GetTopPoint());
                else
                    pPack->ContentAdd(pItemNew);
            }
        }
        else //Player vendors
        {
            if ( pItem->GetAmount() <= amount ) //Buy the whole item
            {
                if ((!pPack->CanContainerHold(pItem, m_pChar)) || (!m_pChar->CanCarry(pItem)))
                    m_pChar->ItemDrop(pItem, m_pChar->GetTopPoint());
                else
                    pPack->ContentAdd(pItem);

                pItem->m_TagDefs.SetNum("NOSAVE", 0, true);
            }
            else
            {
                pItem->SetAmount(pItem->GetAmount() - amount);

                CItem *pItemNew = CItem::CreateDupeItem(pItem);
                pItemNew->m_TagDefs.SetNum("NOSAVE", 0, true);
                pItemNew->SetAmount(amount);
                if ((!pPack->CanContainerHold(pItemNew, m_pChar)) || (!m_pChar->CanCarry(pItemNew)))
                    m_pChar->ItemDrop(pItemNew, m_pChar->GetTopPoint());
                else
                    pPack->ContentAdd(pItemNew);
            }
        }

do_consume:
        pItem->Update();
    }

	if (uiItemBlocked < uiItemCount)
	{
		//Say the message about the bought goods
		tchar* sMsg = Str_GetTemp();
		tchar* pszTemp1 = Str_GetTemp();
		tchar* pszTemp2 = Str_GetTemp();
		snprintf(pszTemp1, Str_TempLength(), g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_HYARE), m_pChar->GetName());
		snprintf(pszTemp2, Str_TempLength(), (fBoss ? g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_S1) : g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_B1)),
			iCostTotal, ((iCostTotal == 1) ? "" : g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CA)));
		snprintf(sMsg, Str_TempLength(), "%s %s %s", pszTemp1, pszTemp2, g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_TY));
		pVendor->Speak(sMsg);
	}

    //Take the gold and add it to the vendor
    if ( !fBoss )
    {
        if (g_Cfg.m_iFeatureTOL & FEATURE_TOL_VIRTUALGOLD) //We have to do gold trade in here.
        {
            m_pChar->m_virtualGold -= iCostTotal;
            m_pChar->UpdateStatsFlag();
        }
        else
        {
            int iGold = m_pChar->GetPackSafe()->ContentConsume( CResourceID(RES_TYPEDEF,IT_GOLD), (int)iCostTotal);
            if (!g_Cfg.m_fPayFromPackOnly && iGold)
                m_pChar->ContentConsume( CResourceID(RES_TYPEDEF,IT_GOLD), iGold);
            pVendor->GetBank()->m_itEqBankBox.m_Check_Amount += (uint)iCostTotal;
        }
    }

    //Close vendor gump
    addVendorClose(pVendor);
    if (iCostTotal > 0 && uiItemBlocked < uiItemCount) //if anything was sold, sound this
        addSound(SOUND_DROP_GOLD1); //Gold sound is better than cloth one, 0x57 is SOUND_USE_CLOTH
}

void CClient::Event_VendorSell_Cheater( int iCode )
{
	ADDTOCALLSTACK("CClient::Event_VendorSell_Cheater");

	// iCode descriptions
	static lpctstr constexpr sm_SellPacketCheats[] =
	{
		"Other",
		"Bad vendor UID",
		"Vendor is off-duty",
		"Bad item UID"
	};

	g_Log.Event(LOGL_WARN|LOGM_CHEAT, "%x:Cheater '%s' is submitting illegal sell packet (%s)\n", GetSocketID(),
		GetAccount()->GetName(),
		sm_SellPacketCheats[iCode]);
	SysMessage(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CANTSELL));
}

void CClient::Event_VendorSell(CChar* pVendor, const VendorItem* items, uint uiItemCount)
{
	ADDTOCALLSTACK("CClient::Event_VendorSell");
	// Player Selling items to the vendor.
	// Done with the selling action.
	if (m_pChar == nullptr || pVendor == nullptr || items == nullptr || uiItemCount <= 0)
		return;

	CItemContainer	*pBank = pVendor->GetBank();
	CItemContainer	*pContStock = pVendor->GetBank( LAYER_VENDOR_STOCK );
	CItemContainer	*pContBuy = pVendor->GetBank( LAYER_VENDOR_BUYS );
	CItemContainer	*pContExtra = pVendor->GetBank( LAYER_VENDOR_EXTRA );
	if ( pBank == nullptr || pContStock == nullptr )
	{
		addVendorClose(pVendor);
		pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_GUARDS));
		return;
	}

	int iConvertFactor = -pVendor->NPC_GetVendorMarkup();

	int iGold = 0;
	bool fShortfall = false;

	for (uint i = 0; i < uiItemCount; ++i)
	{
		CItemVendable * pItem = dynamic_cast <CItemVendable *> (items[i].m_serial.ItemFind());
		if ( pItem == nullptr || pItem->IsValidSaleItem(true) == false )
		{
			Event_VendorSell_Cheater( 0x3 );
			return;
		}

		// Do we still have it ? (cheat check)
		if ( pItem->GetTopLevelObj() != m_pChar )
			continue;

		// Find the valid sell item from vendors stuff.
		CItemVendable * pItemSell = CChar::NPC_FindVendableItem( pItem, pContBuy, pContStock );
		if ( pItemSell == nullptr )
			continue;

		word amount = items[i].m_vcAmount;

		// Now how much did i say i wanted to sell ?
		dword dwPrice = 0;
		if ( pItem->GetAmount() < amount )	// Selling more than i have ?
		{
			amount = pItem->GetAmount();
		}

		// If OVERRIDE.VALUE is define on the script and this NPC buy this item at a specific price, we use this price in priority
		// Else, we calculate the value of the item in the player's backpack
		if (pItemSell->GetKey("OVERRIDE.VALUE", true))
		{
			//Get the price on NPC template
			dwPrice = pItemSell->GetVendorPrice(iConvertFactor,1) * amount; //Check the value of item on NPC template or itemdef
		}
		else
		{
			//Get the price/Value of the real item in the backpack
			dwPrice = pItem->GetVendorPrice(iConvertFactor,1) * amount; //Check the value of the item on the player
		}

		if (( IsTrigUsed(TRIGGER_SELL) ) || ( IsTrigUsed(TRIGGER_ITEMSELL) ))
		{
			CScriptTriggerArgs Args( amount, dwPrice, pVendor );
			if ( pItem->OnTrigger( ITRIG_Sell, this->GetChar(), &Args ) == TRIGRET_RET_TRUE )
				continue;
		}

		// Can vendor afford this ?
		if (dwPrice > pBank->m_itEqBankBox.m_Check_Amount )
		{
			fShortfall = true;
			break;
		}
		pBank->m_itEqBankBox.m_Check_Amount -= dwPrice;

		// give them the appropriate amount of gold.
		iGold += (int)(dwPrice);

		// Take the items from player.
		// Put items in vendor inventory.
		if ( amount >= pItem->GetAmount())
		{
			pItem->RemoveFromView();
			if ( pVendor->IsStatFlag(STATF_PET) && pContExtra )
				pContExtra->ContentAdd(pItem);
			else
				pItem->Delete();

			if ( IsSetOF(OF_VendorStockLimit) )
				pItemSell->ConsumeAmount(pItem->GetAmount());
		}
		else
		{
			if ( pVendor->IsStatFlag(STATF_PET) && pContExtra )
			{
				CItem * pItemNew = CItem::CreateDupeItem(pItem);
				pItemNew->SetAmount(amount);
				pContExtra->ContentAdd(pItemNew);
			}
			pItem->SetAmountUpdate( pItem->GetAmount() - amount );

			if (IsSetOF(OF_VendorStockLimit))
				pItemSell->ConsumeAmount(amount);
		}
	}

	if ( iGold )
	{
		char *z = Str_GetTemp();
		snprintf(z, Str_TempLength(), g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_SELL_TY),
			iGold, (iGold==1) ? "" : g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CA));
		pVendor->Speak(z);

		if ( fShortfall )
			pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_NOMONEY));

		if ( g_Cfg.m_iFeatureTOL & FEATURE_TOL_VIRTUALGOLD )
		{
			m_pChar->m_virtualGold += iGold;
			m_pChar->UpdateStatsFlag();
		}
		else
			m_pChar->AddGoldToPack(iGold, nullptr, false);

		addVendorClose(pVendor);
	}
	else
	{
		if ( fShortfall )
			pVendor->Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_VENDOR_CANTAFFORD));
	}
}

void CClient::Event_Profile( byte fWriteMode, CUID uid, lpctstr pszProfile, int iProfileLen )
{
	ADDTOCALLSTACK("CClient::Event_Profile");
	UnreferencedParameter(iProfileLen);
	// mode = 0 = Get profile, 1 = Set profile
	if ( m_pChar == nullptr )
		return;

	CChar	*pChar = uid.CharFind();
	if ( !pChar || !pChar->m_pPlayer )
		return;

	if ( IsTrigUsed(TRIGGER_PROFILE) )
	{
		if ( pChar->OnTrigger(CTRIG_Profile, m_pChar) == TRIGRET_RET_TRUE )
			return;
	}

	if ( fWriteMode )
	{
		// write stuff to the profile.
		if ( m_pChar != pChar )
		{
			if ( ! IsPriv(PRIV_GM))
				return;
			if ( m_pChar->GetPrivLevel() < pChar->GetPrivLevel())
				return;
		}

		if (pszProfile && !strchr(pszProfile, 0x0A))
			pChar->m_pPlayer->m_sProfile = pszProfile;
	}
	else
	{
		new PacketProfile(this, pChar);
	}
}



void CClient::Event_MailMsg( CUID uid1, CUID uid2 )
{
	ADDTOCALLSTACK("CClient::Event_MailMsg");
	UnreferencedParameter(uid2);
	// NOTE: How do i protect this from spamming others !!!
	// Drag the mail bag to this clients char.
	if ( m_pChar == nullptr )
		return;

	CChar * pChar = uid1.CharFind();
	if ( pChar == nullptr )
	{
		SysMessageDefault( DEFMSG_MSG_MAILBAG_DROP_1 );
		return;
	}

	if ( IsTrigUsed(TRIGGER_USERMAILBAG) )
	{
		if (pChar->OnTrigger(CTRIG_UserMailBag, m_pChar, nullptr) == TRIGRET_RET_TRUE)
			return;
	}

	if ( pChar == m_pChar ) // this is normal (for some reason) at startup.
	{
		return;
	}
	// Might be an NPC ?
	tchar * pszMsg = Str_GetTemp();
	snprintf(pszMsg, Str_TempLength(), g_Cfg.GetDefaultMsg( DEFMSG_MSG_MAILBAG_DROP_2 ), m_pChar->GetName());
	pChar->SysMessage(pszMsg);
}



void CClient::Event_ToolTip( CUID uid )
{
	ADDTOCALLSTACK("CClient::Event_ToolTip");
	CObjBase * pObj = uid.ObjFind();
	if ( pObj == nullptr )
		return;

	if (( IsTrigUsed(TRIGGER_TOOLTIP) ) || (( IsTrigUsed(TRIGGER_ITEMTOOLTIP) )&&(pObj->IsItem())))
	{
		if ( pObj->OnTrigger("@ToolTip", this) == TRIGRET_RET_TRUE )	// CTRIG_ToolTip, ITRIG_ToolTip
			return;
	}

	char *z = Str_GetTemp();
	snprintf(z, Str_TempLength(), "'%s'", pObj->GetName());
	addToolTip(uid.ObjFind(), z);
}

void CClient::Event_PromptResp( lpctstr pszText, size_t len, dword context1, dword context2, dword type, bool fNoStrip )
{
	ADDTOCALLSTACK("CClient::Event_PromptResp");
	if (m_pChar == nullptr)
		return;

	// result of addPrompt
	tchar szText[MAX_TALK_BUFFER];

	if ( Str_Check( pszText ) )
		return;

	CLIMODE_TYPE promptMode = m_Prompt_Mode;
	m_Prompt_Mode = CLIMODE_NORMAL;

	if ( m_Prompt_Uid != context1 )
		return;

	if ( len <= 0 )	// cancel
	{
		szText[0] = 0;
	}
	else
	{
		if ( fNoStrip )	// Str_GetBare will eat unicode characters
			len = Str_CopyLimitNull( szText, pszText, ARRAY_COUNT(szText) );
		else if ( promptMode == CLIMODE_PROMPT_SCRIPT_VERB )
			len = Str_GetBare( szText, pszText, ARRAY_COUNT(szText), "|~=[]{|}~" );
		else
			len = Str_GetBare( szText, pszText, ARRAY_COUNT(szText), "|~,=[]{|}~" );
	}

	lpctstr pszReName = nullptr;
	lpctstr pszPrefix = nullptr;

	switch ( promptMode )
	{
		case CLIMODE_PROMPT_NAME_PET:
			if (Event_SetName(CUID(context1), szText))
				SysMessageDefault(DEFMSG_NPC_PET_RENAME_SUCCESS1);
			return;

		case CLIMODE_PROMPT_GM_PAGE_TEXT:
			// m_Targ_Text
			Event_PromptResp_GMPage(szText);
			return;

		case CLIMODE_PROMPT_VENDOR_PRICE:
			// Setting the vendor price for an item.
			{
				if ( type == 0 || szText[0] == '\0' )	// cancel
					return;
				CChar * pCharVendor = CUID(context2).CharFind();
				if ( pCharVendor )
				{
					pCharVendor->NPC_SetVendorPrice( m_Prompt_Uid.ItemFind(), atoi(szText) );
				}
			}
			return;

		case CLIMODE_PROMPT_NAME_RUNE:
			pszReName = g_Cfg.GetDefaultMsg(DEFMSG_RUNE_NAME);
			pszPrefix = g_Cfg.GetDefaultMsg(DEFMSG_RUNE_TO);
			break;

		case CLIMODE_PROMPT_NAME_KEY:
			pszReName = g_Cfg.GetDefaultMsg(DEFMSG_KEY_NAME);
			pszPrefix = g_Cfg.GetDefaultMsg(DEFMSG_KEY_TO);
			break;

		case CLIMODE_PROMPT_NAME_SHIP:
			pszReName = "Ship";
			pszPrefix = "SS ";
			break;

		case CLIMODE_PROMPT_NAME_SIGN:
			pszReName = "Sign";
			pszPrefix = "";
			break;

		case CLIMODE_PROMPT_STONE_NAME:
			pszReName = g_Cfg.GetDefaultMsg(DEFMSG_STONE_NAME);
			pszPrefix = g_Cfg.GetDefaultMsg(DEFMSG_STONE_FOR);
			break;

		case CLIMODE_PROMPT_TARG_VERB:
			// Send a msg to the pre-tergetted player. "ETARGVERB"
			// m_Prompt_Uid = the target.
			// m_Prompt_Text = the prefix.
			if ( szText[0] != '\0' )
			{
				CObjBase * pObj = m_Prompt_Uid.ObjFind();
				if ( pObj )
				{
					CScript script( m_Prompt_Text, szText );
					pObj->r_Verb( script, this );
				}
			}
			return;

		case CLIMODE_PROMPT_SCRIPT_VERB:
			{
				// CChar * pChar = CUID(context2).CharFind();
				CScript script( m_Prompt_Text, szText );
				if ( m_pChar )
					m_pChar->r_Verb( script, this );
			}
			return;

		default:
			// DEBUG_ERR(( "%x:Unrequested Prompt mode %d\n", m_Socket.GetSocket(), PrvTargMode ));
			SysMessage( g_Cfg.GetDefaultMsg(DEFMSG_MSG_PROMPT_UNEXPECTED) );
			return;
	}

	ASSERT(pszReName);

	CSString sMsg;

	CItem * pItem = m_Prompt_Uid.ItemFind();
	if ( pItem == nullptr || type == 0 || szText[0] == '\0' )
	{
		SysMessagef( g_Cfg.GetDefaultMsg( DEFMSG_MSG_RENAME_CANCEL ), pszReName );
		return;
	}

	if ( g_Cfg.IsObscene( szText ))
	{
		SysMessagef( g_Cfg.GetDefaultMsg( DEFMSG_MSG_RENAME_WNAME ), pszReName, szText );
		return;
	}

	sMsg.Format("%s%s", pszPrefix, szText);
	pItem->SetName(sMsg);
	sMsg.Format(g_Cfg.GetDefaultMsg( DEFMSG_MSG_RENAME_SUCCESS ), pszReName, pItem->GetName());

	SysMessage(sMsg);
}

void CClient::Event_PromptResp_GMPage(lpctstr pszReason)
{
	ADDTOCALLSTACK("CClient::Event_PromptResp_GMPage");
	// Player sent an GM page
	// CLIMODE_PROMPT_GM_PAGE_TEXT

	if (pszReason[0] == '\0')
	{
		SysMessageDefault(DEFMSG_GMPAGE_PROMPT_CANCEL);
		return;
	}

	const CPointMap& pt = m_pChar->GetTopPoint();
	tchar * pszMsg = Str_GetTemp();
	snprintf(pszMsg, Str_TempLength(), g_Cfg.GetDefaultMsg(DEFMSG_GMPAGE_RECEIVED), m_pChar->GetName(), (dword)m_pChar->GetUID(), pt.WriteUsed(), pszReason);
	g_Log.Event(LOGM_NOCONTEXT | LOGM_GM_PAGE, "%s\n", pszMsg);

	CGMPage *pGMPage = nullptr;
	for (auto& sptrGMPage : g_World.m_GMPages)
	{
		if (strcmp(sptrGMPage->GetName(), m_pAccount->GetName()) == 0)
        {
            pGMPage = sptrGMPage.get();
            break;
        }
	}

	if (pGMPage)
	{
		SysMessageDefault(DEFMSG_GMPAGE_UPDATED);
	}
	else
	{
		pGMPage = g_World.m_GMPages.emplace_back(std::make_unique<CGMPage>(m_pAccount->GetName())).get();
		SysMessageDefault(DEFMSG_GMPAGE_SENT);
	}
	pGMPage->m_uidChar = m_pChar->GetUID();
	pGMPage->m_pt = pt;
	pGMPage->m_sReason = pszReason;
	pGMPage->m_time = CWorldGameTime::GetCurrentTime().GetTimeRaw();
	SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_GMPAGE_QUEUE), static_cast<int>(g_World.m_GMPages.size()));

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != nullptr; pClient = it.next())
	{
		if (pClient->IsPriv(PRIV_GM_PAGE))
			pClient->SysMessage(pszMsg);
	}
}


void CClient::Event_Talk_Common(lpctstr pszText)	// PC speech
{
	ADDTOCALLSTACK("CClient::Event_Talk_Common");
	if ( !m_pChar || !m_pChar->m_pPlayer || !m_pChar->m_pArea )
		return;

	// Guards are special
	lpctstr pszMsgGuards = g_Exp.m_VarDefs.GetKeyStr("guardcall");
	if ( !strnicmp(pszMsgGuards, "", 0) )
		pszMsgGuards = "GUARD,GUARDS";
	if ( FindStrWord(pszText, pszMsgGuards) > 0 )
		m_pChar->CallGuards();

	// Are we in a region that can hear ?
	if ( m_pChar->m_pArea->GetResourceID().IsUIDItem() )
	{
		CItemMulti *pItemMulti = dynamic_cast<CItemMulti *>(m_pChar->m_pArea->GetResourceID().ItemFindFromResource());
		if ( pItemMulti )
			pItemMulti->OnHearRegion(pszText, m_pChar);
	}

	// Are there items on the ground that might hear u ?
	CSector *pSector = m_pChar->GetTopSector();
	if ( pSector->HasListenItems() )
		pSector->OnHearItem(m_pChar, pszText);

	// Find an NPC that may have heard us.
	CChar *pChar = nullptr;
	CChar *pCharAlt = nullptr;
	size_t i = 0;
    bool bGhostSpeak = m_pChar->IsSpeakAsGhost();
    int iFullDist = UO_MAP_VIEW_SIGHT;
    bool fIgnoreLOS = (g_Cfg.m_iNPCDistanceHear < 0);
    if (g_Cfg.m_iNPCDistanceHear != 0)
    {
        iFullDist = abs(g_Cfg.m_iNPCDistanceHear);
    }

    //Reduce NPC hear distance for non pets
    int iAltDist = iFullDist;

	auto AreaChars = CWorldSearchHolder::GetInstance(m_pChar->GetTopPoint(), iFullDist); // Search for the iFullDist, as it can be overriden in sphere.
	for (;;)
	{
		pChar = AreaChars->GetChar();

        //No more Chars to check
		if ( !pChar )
			break;

        //Has Communication Crystal Flag on?
		if ( pChar->IsStatFlag(STATF_COMM_CRYSTAL) )
		{
			for (CSObjContRec* pObjRec : pChar->GetIterationSafeCont())
			{
				CItem* pItem = static_cast<CItem*>(pObjRec);
                if (pItem->CanHear()) {
                    pItem->OnHear(pszText, m_pChar);
                }
			}
		}

        //Skik myself
        if (pChar == m_pChar)
        {
            continue;
        }
        //Skip non NPCs
		if ( !pChar->m_pNPC )
        {
            continue;
        }
        // Skip NPCs that can't understand ghosts if you are dead
		if ( bGhostSpeak && !pChar->CanUnderstandGhost() )
        {
            continue;
        }
        /*
		Skip Vendors that are too far when buying or selling
		No need to use this check anymore, NPCs should be able to hear up to the value of NPCDistanceHear setting in the .ini (default 4 tiles).
        if (pChar->NPC_IsVendor() && (m_pChar->CanTouch(pChar) == false) && (FindStrWord(pszText, "buy,sell") > 0))
        {
            continue;
        }
		*/

		int iDist = m_pChar->GetTopDist3D(pChar);

		//Can't see or too far, Can't hear!
		if (((!m_pChar->CanSeeLOS(pChar)) && (!fIgnoreLOS)) || (iDist > iFullDist))
			continue;

		bool bNamed = false;
		i = 0;
		if ( !strnicmp(pszText, "ALL ", 4) )
        {
			i = 4;
        }
		else
		{
			// Named the char specifically?
			//If i is 0 that means we have used a KEYWORD without a name or that we did not find any NPCs with that name.
			i = pChar->NPC_OnHearName(pszText);
			bNamed = (bool)i;
		}
		if ( i > 0 )
		{
            while ( IsWhitespace(pszText[i]) )
            {
                ++i;
            }

			if ( (pChar->NPC_IsOwnedBy(m_pChar)) && (pChar->NPC_OnHearPetCmd(pszText + i, m_pChar, !bNamed)) )
			{
                //Stop for single pet target or named
				if ( bNamed || (GetTargMode() == CLIMODE_TARG_PET_CMD) )
					return;

                // the command might apply to others pets
				continue;
			}

			if ( bNamed )
				break;
		}

		// Pick closest NPC?
		if (iDist <= iAltDist)
		{
			pCharAlt = pChar;
			iAltDist = iDist;
		}
        // already talking to him
		/* No need of this check too, it creates problem when multiple NPCs are nearby.
		else if ((pChar->Skill_GetActive() == NPCACT_TALK) && (pChar->m_Act_UID == m_pChar->GetUID()))
		{
			pCharAlt = pChar;
            break;
		}
       */
        // NPC's with special key words ?
        if (pChar->m_pNPC)
        {
            if (pChar->m_pNPC->m_Brain == NPCBRAIN_BANKER)
            {
                if (FindStrWord(pszText, "BANK") > 0)
                    break;
            }
        }
	}

	if ( !pChar )
	{
		i = 0;
		pChar = pCharAlt;
		if ( !pChar )
			return;	// no one heard it.
	}

	// The char hears you say this.
	pChar->NPC_OnHear(&pszText[i], m_pChar);
}

// PC speech: response to ASCII speech request
void CClient::Event_Talk( lpctstr pszText, HUE_TYPE wHue, TALKMODE_TYPE mode, bool fNoStrip)
{
	ADDTOCALLSTACK("CClient::Event_Talk");

	CAccount *pAccount = GetAccount();
	ASSERT(pAccount && m_pChar && m_pChar->m_pPlayer);

	if ( mode < 0 || mode > 14 ) // Less or greater is an exploit
		return;

	// These modes are server->client only
	if ( mode == 1 || mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 10 || mode == 11 || mode == 12 )
		return;

	if ( (wHue < 2) || (wHue > HUE_DYE_HIGH) )
		wHue = HUE_SAY_DEF;

	pAccount->m_lang.Set( nullptr );	// default
	if ( (mode == TALKMODE_SAY) && !m_pChar->m_SpeechHueOverride )
		m_pChar->m_pPlayer->m_SpeechHue = wHue;
	else if ( (mode == TALKMODE_EMOTE) && !m_pChar->m_EmoteHueOverride )
		m_pChar->m_pPlayer->m_EmoteHue = wHue;

	// Rip out the unprintables first
	tchar szText[MAX_TALK_BUFFER];
	size_t len;

	if ( fNoStrip )
	{
		// The characters in Unicode speech don't need to be filtered
		Str_CopyLimitNull( szText, pszText, MAX_TALK_BUFFER );
		len = strlen( szText );
	}
	else
	{
		tchar szTextG[MAX_TALK_BUFFER];
		Str_CopyLimitNull( szTextG, pszText, MAX_TALK_BUFFER );
		len = Str_GetBare( szText, szTextG, sizeof(szText)-1 );
	}

	if ( len <= 0 )
		return;

	pszText = szText;
	GETNONWHITESPACE(pszText);

	if ( !Event_Command(pszText,mode) )
	{
		bool fCancelSpeech = false;
		tchar z[MAX_TALK_BUFFER];

		if ( m_pChar->OnTriggerSpeech(false, pszText, m_pChar, mode, wHue) )
			fCancelSpeech = true;

		if ( g_Log.IsLoggedMask(LOGM_PLAYER_SPEAK) )
		{
			g_Log.Event(LOGM_PLAYER_SPEAK|LOGM_NOCONTEXT, "%x:'%s' Says '%s' mode=%d%s\n",
				GetSocketID(), m_pChar->GetName(), pszText, mode, fCancelSpeech ? " (muted)" : "");
		}

		// Guild and Alliance mode will not pass this
		if ( mode == 13 || mode == 14 )
			return;

		Str_CopyLimitNull(z, pszText, sizeof(z));

		if ( g_Cfg.m_fSuppressCapitals )
		{
			int chars = (int)strlen(z);
			int capitals = 0;
			int i = 0;
			for ( i = 0; i < chars; i++ )
				if (( z[i] >= 'A' ) && ( z[i] <= 'Z' ))
					capitals++;

			if (( chars > 5 ) && ((( capitals * 100 )/chars) > 75 ))
			{							// 75% of chars are in capital letters. lowercase it
				for ( i = 1; i < chars; i++ )				// instead of the 1st char
					if (( z[i] >= 'A' ) && ( z[i] <= 'Z' ))
						z[i] += 0x20;
			}
		}

		if ( !fCancelSpeech && ( len <= 128 ) ) // From this point max 128 chars
		{
			// For both client ASCII and Unicode speech requests, Sphere sends a Unicode speech.
			m_pChar->SpeakUTF8(z, wHue, (TALKMODE_TYPE)mode, m_pChar->m_fonttype, GetAccount()->m_lang);
			Event_Talk_Common(static_cast<tchar*>(z));
		}
	}
}

// PC speech: response to Unicode speech request
void CClient::Event_TalkUNICODE(nachar* wszText, int iTextLen, HUE_TYPE wHue, TALKMODE_TYPE mMode, FONT_TYPE font, lpctstr pszLang )
{
	ADDTOCALLSTACK("CClient::Event_TalkUNICODE");
	// Get the text in wide bytes.
	// ENU = English
	// FRC = French

	CAccount *pAccount = GetAccount();
    ASSERT(pAccount && m_pChar && m_pChar->m_pPlayer);

	if ( iTextLen <= 0 )
		return;

	if ( mMode < 0 || mMode > 14 ) // Less or greater is an exploit
		return;

	// These modes are server->client only
	if ( mMode == 1 || mMode == 3 || mMode == 4 || mMode == 5 || mMode == 6 || mMode == 7 || mMode == 10 || mMode == 11 || mMode == 12 )
		return;

	if ((wHue < 2) || (wHue > HUE_DYE_HIGH))
		wHue = HUE_SAY_DEF;

	pAccount->m_lang.Set(pszLang);
	if ( (mMode == TALKMODE_SAY) && (!m_pChar->m_SpeechHueOverride) )
		m_pChar->m_pPlayer->m_SpeechHue = wHue;
	else if ( (mMode == TALKMODE_EMOTE) && (!m_pChar->m_EmoteHueOverride) )
		m_pChar->m_pPlayer->m_EmoteHue = wHue;

	tchar szText[MAX_TALK_BUFFER];
	const nachar * puText = wszText;

	int iLen = CvtNETUTF16ToSystem( szText, sizeof(szText), wszText, iTextLen );
	if ( iLen <= 0 )
		return;

	tchar* pszText = szText;
	GETNONWHITESPACE(pszText);

	if ( !Event_Command(pszText, mMode) )
	{
		bool fCancelSpeech	= false;

		if ( m_pChar->OnTriggerSpeech( false, pszText, m_pChar, mMode, wHue) )
			fCancelSpeech	= true;

		if ( g_Log.IsLoggedMask(LOGM_PLAYER_SPEAK) )
		{
			g_Log.Event(LOGM_PLAYER_SPEAK|LOGM_NOCONTEXT, "%x:'%s' Says UNICODE '%s' '%s' mode=%d%s\n", GetSocketID(),
				m_pChar->GetName(), pAccount->m_lang.GetStr(), pszText, mMode, fCancelSpeech ? " (muted)" : "" );
		}

		// Guild and Alliance mode will not pass this.
		if ( mMode == 13 || mMode == 14 )
			return;

		if ( g_Cfg.m_fSuppressCapitals )
		{
			int chars = (int)strlen(szText);
			int capitals = 0;
			int i = 0;
			for ( i = 0; i < chars; i++ )
				if (( szText[i] >= 'A' ) && ( szText[i] <= 'Z' ))
					capitals++;

			if (( chars > 5 ) && ((( capitals * 100 )/chars) > 75 ))
			{							// 75% of chars are in capital letters. lowercase it
				for ( i = 1; i < chars; i++ )				// instead of the 1st char
					if (( szText[i] >= 'A' ) && ( szText[i] <= 'Z' ))
						szText[i] += 0x20;

				iLen = CvtSystemToNETUTF16(wszText, iTextLen, szText, (int)chars);
			}
		}

		if ( !fCancelSpeech && ( iLen <= 128 ) ) // From this point max 128 chars
		{
			m_pChar->SpeakUTF8Ex(puText, wHue, mMode, font, pAccount->m_lang);
			Event_Talk_Common(pszText);
		}
	}
}

bool CClient::Event_SetName( CUID uid, const char * pszCharName )
{
	ADDTOCALLSTACK("CClient::Event_SetName");
	// Set the name in the character status window.
	CChar * pChar = uid.CharFind();
	if (!pChar || !m_pChar)
		return false;

   if ( Str_CheckName(pszCharName) || !strlen(pszCharName) )
		return false;

	// Do we have the right to do this ?
	if ( (m_pChar == pChar) || !pChar->IsOwnedBy( m_pChar, true ) )
		return false;
	if ( FindTableSorted( pszCharName, sm_szCmd_Redirect, ARRAY_COUNT(sm_szCmd_Redirect) ) >= 0 )
		return false;
	if ( FindTableSorted( pszCharName, CCharNPC::sm_szVerbKeys, 14 ) >= 0 )
		return false;
	if ( g_Cfg.IsObscene(pszCharName) )
		return false;

	if ( IsTrigUsed(TRIGGER_RENAME) )
	{
		CScriptTriggerArgs args;
		args.m_pO1 = pChar;
		args.m_s1 = pszCharName;
		if ( m_pChar->OnTrigger(CTRIG_Rename, this, &args) == TRIGRET_RET_TRUE )
			return false;
	}
	if (pChar->IsOwnedBy(m_pChar))
		SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_NPC_PET_RENAME_SUCCESS2), pChar->GetName(), pszCharName);

	pChar->SetName(pszCharName);
	return true;
}

void CDialogResponseArgs::AddText( word id, lpctstr pszText )
{
    m_TextArray.push_back(new TResponseString(id, pszText));
}

lpctstr CDialogResponseArgs::GetName() const
{
	return "ARGD";
}

bool CDialogResponseArgs::r_WriteVal( lpctstr ptcKey, CSString &sVal, CTextConsole * pSrc, bool fNoCallParent, bool fNoCallChildren )
{
    UnreferencedParameter(fNoCallChildren);
	ADDTOCALLSTACK("CDialogResponseArgs::r_WriteVal");
	EXC_TRY("WriteVal");
	if ( ! strnicmp( ptcKey, "ARGCHK", 6 ))
	{
		// CSTypedArray <dword,dword> m_CheckArray;
		ptcKey += 6;
		SKIP_SEPARATORS(ptcKey);

        const size_t iQty = m_CheckArray.size();
		if ( ptcKey[0] == '\0' )
		{
			sVal.FormatSTVal(iQty);
			return true;
		}
		else if ( ! strnicmp( ptcKey, "ID", 2) )
		{
			ptcKey += 2;

			if ( iQty > 0 && m_CheckArray[0] )
				sVal.FormatVal( m_CheckArray[0] );
			else
				sVal.FormatVal( -1 );

			return true;
		}

        const dword dwNum = Exp_GetDWSingle( ptcKey );
		SKIP_SEPARATORS(ptcKey);
		for ( uint i = 0; i < iQty; ++i )
		{
			if ( dwNum == m_CheckArray[i] )
			{
                sVal.SetValTrue();
				return true;
			}
		}
		sVal.SetValFalse();
		return true;
	}
	if ( ! strnicmp( ptcKey, "ARGTXT", 6 ))
	{
		ptcKey += 6;
		SKIP_SEPARATORS(ptcKey);

        const size_t iQty = m_TextArray.size();
		if ( ptcKey[0] == '\0' )
		{
			sVal.FormatSTVal(iQty);
			return true;
		}

        const dword dwNum = Exp_GetDWSingle( ptcKey );
		SKIP_SEPARATORS(ptcKey);

		for ( uint i = 0; i < iQty; ++i )
		{
			if ( dwNum == m_TextArray[i]->m_ID )
			{
				sVal = m_TextArray[i]->m_sText;
				return true;
			}
		}
		sVal.Clear();
		return false;
	}
	return (fNoCallParent ? false : CScriptTriggerArgs::r_WriteVal( ptcKey, sVal, pSrc, false ));
	EXC_CATCH;

	EXC_DEBUG_START;
	EXC_ADD_KEYRET(pSrc);
	EXC_DEBUG_END;
	return false;
}

bool CClient::Event_DoubleClick( CUID uid, bool fMacro, bool fTestTouch, bool fScript )
{
	ADDTOCALLSTACK("CClient::Event_DoubleClick");
	// Try to use the object in some way.
	// will trigger a OnTarg_Use_Item() usually.
	// fMacro = ALTP vs dbl click. no unmount.

	// Allow some static in game objects to have function?
	// Not possible with dclick.
	if ( m_pChar == nullptr )
		return false;

	CObjBase * pObj = uid.ObjFind();
	if ( !pObj || (fTestTouch && !m_pChar->CanSee(pObj) && !(pObj->Can( CAN_I_FORCEDC )) ) )
	{
		addObjectRemoveCantSee(uid, "the target");
		return false;
	}

	// Face the object
	if ( !IsSetOF(OF_NoDClickTurn) && (pObj->GetTopLevelObj() != m_pChar) )
	{
		SetTargMode();
		m_Targ_UID = uid;
		m_pChar->UpdateDir(pObj);
	}

	if ( pObj->IsItem() )
		return Cmd_Use_Item(static_cast<CItem *>(pObj), fTestTouch, fScript);

	CChar * pChar = static_cast<CChar *>(pObj);
	if ( IsTrigUsed(TRIGGER_DCLICK) || IsTrigUsed(TRIGGER_CHARDCLICK) )
	{
		if ( pChar->OnTrigger(CTRIG_DClick, m_pChar) == TRIGRET_RET_TRUE )
			return true;
	}

	if ( !fMacro )
	{
		if ( pChar == m_pChar )
		{
			if ( pChar->IsStatFlag(STATF_ONHORSE) )
			{
				// in war mode not to drop from horse accidentally we need this check
				// Should also check for STATF_WAR in case someone starts fight and runs away.
				if ( !IsSetCombatFlags(COMBAT_DCLICKSELF_UNMOUNTS) && pChar->IsStatFlag(STATF_WAR) && pChar->Memory_FindTypes(MEMORY_FIGHT) )
				{
					addCharPaperdoll(pChar);
					return true;
				}
				else if ( pChar->Horse_UnMount() )
					return true;
			}
		}

		if ( pChar->m_pNPC && (pChar->GetNPCBrainGroup() != NPCBRAIN_HUMAN) )
		{
			if ( m_pChar->Horse_Mount(pChar) )
				return true;

			switch ( pChar->GetID() )
			{
				case CREID_HORSE_PACK:
				case CREID_LLAMA_PACK:
					return Cmd_Use_Item(pChar->GetPackSafe(), fTestTouch);	// pack animals open container
				default:
					if ( IsPriv(PRIV_GM) )
						return Cmd_Use_Item(pChar->GetPackSafe(), false);	// snoop the creature
					return false;
			}
		}
	}

	addCharPaperdoll(pChar);
	return true;
}

void CClient::Event_SingleClick( CUID uid )
{
	ADDTOCALLSTACK("CClient::Event_SingleClick");
	// The client is doing a single click on obj.
	// Also called to show incoming char names
	// on screen (including ALLNAMES macro).
	//
	// PS: Clients using tooltips don't send single click
	//	   requests when clicking on obj.

	if ( m_pChar == nullptr )
		return;

	CObjBase * pObj = uid.ObjFind();
	if ( !m_pChar->CanSee(pObj) )
	{
		// ALLNAMES makes this happen as we are running thru an area,
		// so display no msg. Do not use addObjectRemoveCantSee()
		addObjectRemove(uid);
		return;
	}

	if ( IsTrigUsed(TRIGGER_CLICK) || (IsTrigUsed(TRIGGER_ITEMCLICK) && pObj->IsItem()) || (IsTrigUsed(TRIGGER_CHARCLICK) && pObj->IsChar()) )
	{
		CScriptTriggerArgs Args(this);
		// The "@Click" trigger str should be the same between items and chars...
		if ( pObj->OnTrigger(CChar::sm_szTrigName[CTRIG_Click], m_pChar, &Args) == TRIGRET_RET_TRUE )	// CTRIG_Click, ITRIG_Click
			return;
	}

	if ( pObj->IsItem() )
	{
		addItemName(dynamic_cast<CItem *>(pObj));
		return;
	}

	if ( pObj->IsChar() )
	{
		addCharName(dynamic_cast<CChar *>(pObj));
		return;
	}

	SysMessagef("Bogus item uid=0%x?", (dword)uid);
}

void CClient::Event_Target(dword context, CUID uid, CPointMap pt, byte flags, ITEMID_TYPE id)
{
	ADDTOCALLSTACK("CClient::Event_Target");
	// XCMD_Target
	// If player clicks on something with the targetting cursor
	// Assume addTarget was called before this.
	// NOTE: Make sure they can actually validly trarget this item !
	if (m_pChar == nullptr)
		return;

	if (context != (dword)GetTargMode())
	{
		// unexpected context
		if (context != 0 && (pt.m_x != -1 || uid.GetPrivateUID() != 0))
			SysMessage( g_Cfg.GetDefaultMsg(DEFMSG_MSG_TARG_UNEXPECTED) );

		return;
	}

	if (pt.m_x == -1 && uid.GetPrivateUID() == 0)
	{
		// cancelled
		SetTargMode();
		return;
	}

	CLIMODE_TYPE prevmode = GetTargMode();
	ClearTargMode();

	if (GetNetState()->isClientKR() && (flags & 0xA0))
		uid = m_Targ_Last;

	CObjBase* pTarget = uid.ObjFind();
	if (IsPriv(PRIV_GM))
	{
		if (uid.IsValidUID() && pTarget == nullptr)
		{
			addObjectRemoveCantSee(uid, "the target");
			return;
		}
	}
	else
	{
		if (uid.IsValidUID())
		{
            if (CChar *pTargetChar = dynamic_cast<CChar*>(pTarget))
            {
                if (pTargetChar->Can(CAN_C_NONSELECTABLE))
                    return;
            }
			if (m_pChar->CanSee(pTarget) == false)
			{
				addObjectRemoveCantSee(uid, "the target");
				return;
			}
		}
		else
		{
			// the point must be valid
			if (m_pChar->GetTopDistSight(pt) > m_pChar->GetVisualRange())
				return;
		}
	}

	if (pTarget != nullptr)
	{
		// remove the last existing target
		m_Targ_Last = uid;

		// point inside a container is not really meaningful here
		pt = pTarget->GetTopLevelObj()->GetTopPoint();
	}

	switch (prevmode)
	{
		// GM stuff.
		case CLIMODE_TARG_OBJ_SET:			OnTarg_Obj_Set( pTarget ); break;
		case CLIMODE_TARG_OBJ_INFO:			OnTarg_Obj_Info( pTarget, pt, id );  break;
		case CLIMODE_TARG_OBJ_FUNC:			OnTarg_Obj_Function( pTarget, pt, id );  break;

		case CLIMODE_TARG_UNEXTRACT:		OnTarg_UnExtract( pTarget, pt ); break;
		case CLIMODE_TARG_ADDCHAR:			OnTarg_Char_Add( pTarget, pt ); break;
		case CLIMODE_TARG_ADDITEM:			OnTarg_Item_Add( pTarget, pt ); break;
		case CLIMODE_TARG_LINK:				OnTarg_Item_Link( pTarget ); break;
		case CLIMODE_TARG_TILE:				OnTarg_Tile( pTarget, pt );  break;

		// Player stuff.
		case CLIMODE_TARG_SKILL:			OnTarg_Skill( pTarget ); break;
		case CLIMODE_TARG_SKILL_MAGERY:     OnTarg_Skill_Magery( pTarget, pt ); break;
		case CLIMODE_TARG_SKILL_HERD_DEST:  OnTarg_Skill_Herd_Dest( pTarget, pt ); break;
		case CLIMODE_TARG_SKILL_POISON:		OnTarg_Skill_Poison( pTarget ); break;
		case CLIMODE_TARG_SKILL_PROVOKE:	OnTarg_Skill_Provoke( pTarget ); break;

		case CLIMODE_TARG_REPAIR:			m_pChar->Use_Repair( uid.ItemFind() ); break;
		case CLIMODE_TARG_PET_CMD:			OnTarg_Pet_Command( pTarget, pt ); break;
		case CLIMODE_TARG_PET_STABLE:		OnTarg_Pet_Stable( uid.CharFind() ); break;

		case CLIMODE_TARG_USE_ITEM:			OnTarg_Use_Item( pTarget, pt, id );  break;
		case CLIMODE_TARG_STONE_RECRUIT:	OnTarg_Stone_Recruit( uid.CharFind() );  break;
		case CLIMODE_TARG_STONE_RECRUITFULL:OnTarg_Stone_Recruit( uid.CharFind(), true ); break;
		case CLIMODE_TARG_PARTY_ADD:		OnTarg_Party_Add( uid.CharFind() );  break;

		default:							break;
	}
}

void CClient::Event_AOSPopupMenuRequest( dword uid ) //construct packet after a client request
{
	ADDTOCALLSTACK("CClient::Event_AOSPopupMenuRequest");
	CUID uObj(uid);
	CObjBaseTemplate *pObj = uObj.ObjFind();
	if ( !m_pChar || m_pChar->IsStatFlag(STATF_DEAD) || !CanSee(pObj) )
		return;
	if ( !IsSetOF(OF_NoContextMenuLOS) && !m_pChar->CanSeeLOS(pObj) )
		return;

	if ( m_pPopupPacket != nullptr )
	{
		DEBUG_ERR(("New popup packet being formed before previous one has been released.\n"));

		delete m_pPopupPacket;
		m_pPopupPacket = nullptr;
	}
	m_pPopupPacket = new PacketDisplayPopup(this, uObj);

	CScriptTriggerArgs Args;
	bool fPreparePacket = false;
	CItem *pItem = uObj.ItemFind();
	CChar *pChar = uObj.CharFind();

	if ( pItem )
	{
		if ( IsTrigUsed(TRIGGER_CONTEXTMENUREQUEST) || IsTrigUsed(TRIGGER_ITEMCONTEXTMENUREQUEST) )
		{
			Args.m_iN1 = 1;
			pItem->OnTrigger(ITRIG_ContextMenuRequest, GetChar(), &Args);
			fPreparePacket = true;		// there's no hardcoded stuff for items
		}
		else
		{
			delete m_pPopupPacket;
			m_pPopupPacket = nullptr;
			return;
		}
	}
	else if ( pChar )
	{
		if ( IsTrigUsed(TRIGGER_CONTEXTMENUREQUEST) )
		{
			Args.m_iN1 = 1;
			TRIGRET_TYPE iRet = pChar->OnTrigger(CTRIG_ContextMenuRequest, GetChar(), &Args);
			if ( iRet == TRIGRET_RET_TRUE )
				fPreparePacket = true;
		}
	}
	else
	{
		delete m_pPopupPacket;
		m_pPopupPacket = nullptr;
		return;
	}

	if ( pChar && !fPreparePacket )
	{

		if (pChar->IsPlayableCharacter())
			m_pPopupPacket->addOption(POPUP_PAPERDOLL, 6123, POPUPFLAG_COLOR, 0xFFFF);

		if (pChar->m_pNPC)
		{
			if (pChar->NPC_IsVendor())
			{
				if (pChar->m_pNPC->m_Brain == NPCBRAIN_BANKER)
					m_pPopupPacket->addOption(POPUP_BANKBOX, 6105, POPUPFLAG_COLOR, 0xFFFF);

				m_pPopupPacket->addOption(POPUP_VENDORBUY, 6103, POPUPFLAG_COLOR, 0xFFFF);
				m_pPopupPacket->addOption(POPUP_VENDORSELL, 6104, POPUPFLAG_COLOR, 0xFFFF);

				for (unsigned int i = 0; i < g_Cfg.m_iMaxSkill; ++i)
				{
					if (!g_Cfg.m_SkillIndexDefs.valid_index(i))
						continue;
					if (i == SKILL_SPELLWEAVING)
						continue;
					if (g_Cfg.IsSkillFlag((SKILL_TYPE)i, SKF_DISABLED))
						continue;

					ushort wSkillNPC = pChar->Skill_GetBase( (SKILL_TYPE)i );
					if (wSkillNPC < 300)
						continue;

					ushort wSkillPlayer = m_pChar->Skill_GetBase( (SKILL_TYPE)i );
					word wFlag = ((wSkillPlayer >= g_Cfg.m_iTrainSkillMax) || (wSkillPlayer >= (wSkillNPC * g_Cfg.m_iTrainSkillPercent) / 100)) ? POPUPFLAG_LOCKED : POPUPFLAG_COLOR;
					m_pPopupPacket->addOption( (word)(POPUP_TRAINSKILL + i), 6000 + i, wFlag, 0xFFFF);
				}

				if (pChar->m_pNPC->m_Brain == NPCBRAIN_STABLE)
				{
					m_pPopupPacket->addOption(POPUP_STABLESTABLE, 6126, POPUPFLAG_COLOR, 0xFFFF);
					m_pPopupPacket->addOption(POPUP_STABLERETRIEVE, 6127, POPUPFLAG_COLOR, 0xFFFF);
				}
			}
			else
			{
				word iEnabled = pChar->IsStatFlag(STATF_DEAD) ? POPUPFLAG_LOCKED : POPUPFLAG_COLOR;
				if (( pChar->IsOwnedBy(m_pChar, false) && ((pChar->m_pNPC->m_Brain != NPCBRAIN_BERSERK)) ) || m_pChar->IsPriv(PRIV_GM))
				{
					CREID_TYPE id = pChar->GetID();

					m_pPopupPacket->addOption(POPUP_PETGUARD, 6107, iEnabled, 0xFFFF);
					m_pPopupPacket->addOption(POPUP_PETFOLLOW, 6108, POPUPFLAG_COLOR, 0xFFFF);

					bool bBackpack = (id == CREID_LLAMA_PACK || id == CREID_HORSE_PACK || id == CREID_GIANT_BEETLE);
					if (bBackpack)
                        //For information, PET drop do not exist on OSI anymore. Could be check if it work on dragon and horde minions
						m_pPopupPacket->addOption(POPUP_PETDROP, 6109, iEnabled, 0xFFFF);

					m_pPopupPacket->addOption(POPUP_PETKILL, 6111, iEnabled, 0xFFFF);
					m_pPopupPacket->addOption(POPUP_PETSTOP, 6112, POPUPFLAG_COLOR, 0xFFFF);
					m_pPopupPacket->addOption(POPUP_PETSTAY, 6114, POPUPFLAG_COLOR, 0xFFFF);

					if (GetNetState()->isClientVersionNumber(MINCLIVER_NEWDAMAGE))
						m_pPopupPacket->addOption(POPUP_PETRENAME, 1115557, POPUPFLAG_COLOR, 0xFFFF);

					if (!pChar->IsStatFlag(STATF_CONJURED))
					{
						m_pPopupPacket->addOption(POPUP_PETFRIEND_ADD, 6110, iEnabled, 0xFFFF);
						m_pPopupPacket->addOption(POPUP_PETFRIEND_REMOVE, 6099, iEnabled, 0xFFFF);
						m_pPopupPacket->addOption(POPUP_PETTRANSFER, 6113, POPUPFLAG_COLOR, 0xFFFF);
					}

					m_pPopupPacket->addOption(POPUP_PETRELEASE, 6118, POPUPFLAG_COLOR, 0xFFFF);

					if (bBackpack)
						m_pPopupPacket->addOption(POPUP_BACKPACK, 6145, iEnabled, 0xFFFF);
				}
				else if (pChar->Memory_FindObjTypes(m_pChar, MEMORY_FRIEND))
				{
					m_pPopupPacket->addOption(POPUP_PETFOLLOW, 6108, iEnabled, 0xFFFF);
					m_pPopupPacket->addOption(POPUP_PETSTOP, 6112, iEnabled, 0xFFFF);
					m_pPopupPacket->addOption(POPUP_PETSTAY, 6114, iEnabled, 0xFFFF);
				}
				else if (!pChar->IsStatFlag(STATF_PET) && (pChar->Skill_GetBase(SKILL_TAMING) > 0))
					m_pPopupPacket->addOption(POPUP_TAME, 6130, POPUPFLAG_COLOR, 0xFFFF);
			}
		}
		else if (pChar == m_pChar)
		{
			m_pPopupPacket->addOption(POPUP_BACKPACK, 6145, POPUPFLAG_COLOR, 0xFFFF);
			if (GetNetState()->isClientVersionNumber(MINCLIVER_STATUS_V6))
			{
				if (pChar->GetDefNum("REFUSETRADES", true))
					m_pPopupPacket->addOption(POPUP_TRADE_ALLOW, 1154112, POPUPFLAG_COLOR, 0xFFFF);
				else
					m_pPopupPacket->addOption(POPUP_TRADE_REFUSE, 1154113, POPUPFLAG_COLOR, 0xFFFF);
			}

			if (GetNetState()->isClientVersionNumber(MINCLIVER_GLOBALCHAT) && (g_Cfg.m_iChatFlags & CHATF_GLOBALCHAT))
			{
				if (pChar->m_pPlayer->m_fRefuseGlobalChatRequests)
					m_pPopupPacket->addOption(POPUP_GLOBALCHAT_ALLOW, 1158415, POPUPFLAG_COLOR, 0xFFFF);
				else
					m_pPopupPacket->addOption(POPUP_GLOBALCHAT_REFUSE, 1158416, POPUPFLAG_COLOR, 0xFFFF);
			}
		}
		else
		{
			if (m_pChar->m_pParty == nullptr && pChar->m_pParty == nullptr)
				m_pPopupPacket->addOption(POPUP_PARTY_ADD, 197, POPUPFLAG_COLOR, 0xFFFF);
			else if (m_pChar->m_pParty != nullptr && m_pChar->m_pParty->IsPartyMaster(m_pChar))
			{
				if (pChar->m_pParty == nullptr)
					m_pPopupPacket->addOption(POPUP_PARTY_ADD, 197, POPUPFLAG_COLOR, 0xFFFF);
				else if (pChar->m_pParty == m_pChar->m_pParty)
					m_pPopupPacket->addOption(POPUP_PARTY_REMOVE, 198, POPUPFLAG_COLOR, 0xFFFF);
			}

			if (GetNetState()->isClientVersionNumber(MINCLIVER_TOL) && m_pChar->GetDist(pChar) <= 2)
				m_pPopupPacket->addOption(POPUP_TRADE_OPEN, 1077728, POPUPFLAG_COLOR, 0xFFFF);
		}

		if ( (Args.m_iN1 != 1) && (IsTrigUsed(TRIGGER_CONTEXTMENUREQUEST)) )
		{
			Args.m_iN1 = 2;
			pChar->OnTrigger(CTRIG_ContextMenuRequest, GetChar(), &Args);
		}
	}

	if ( m_pPopupPacket->getOptionCount() <= 0 )
	{
		delete m_pPopupPacket;
		m_pPopupPacket = nullptr;
		return;
	}

	m_pPopupPacket->finalise();
	m_pPopupPacket->push(this);
	m_pPopupPacket = nullptr;
}

void CClient::Event_AOSPopupMenuSelect(dword uid, word EntryTag)	//do something after a player selected something from a pop-up menu
{
	ADDTOCALLSTACK("CClient::Event_AOSPopupMenuSelect");
	if ( !m_pChar || !EntryTag )
		return;

	CUID uObj(uid);
	CObjBase *pObj = uObj.ObjFind();
	if ( !CanSee(pObj) )
		return;
	if ( !IsSetOF(OF_NoContextMenuLOS) && !m_pChar->CanSeeLOS(pObj) )
		return;

	CScriptTriggerArgs Args;
	CItem *pItem = uObj.ItemFind();
	if ( pItem )
	{
		if ( IsTrigUsed(TRIGGER_CONTEXTMENUSELECT) || IsTrigUsed(TRIGGER_ITEMCONTEXTMENUSELECT) )
		{
			Args.m_iN1 = EntryTag;
			pItem->OnTrigger(ITRIG_ContextMenuSelect, GetChar(), &Args);
		}
		return;		// there's no hardcoded stuff for items
	}

	CChar *pChar = uObj.CharFind();
	if ( !pChar )
		return;

	if ( IsTrigUsed(TRIGGER_CONTEXTMENUSELECT) )
	{
		Args.m_iN1 = EntryTag;
		if ( pChar->OnTrigger(CTRIG_ContextMenuSelect, GetChar(), &Args) == TRIGRET_RET_TRUE )
			return;
	}

	if ( pChar->m_pNPC )
	{
		switch ( EntryTag )
		{
			case POPUP_BANKBOX:
				if ( pChar->m_pNPC->m_Brain == NPCBRAIN_BANKER )
					pChar->NPC_OnHear("bank", m_pChar);
				break;

			case POPUP_VENDORBUY:
				if ( pChar->NPC_IsVendor() )
					pChar->NPC_OnHear("buy", m_pChar);
				break;

			case POPUP_VENDORSELL:
				if ( pChar->NPC_IsVendor() )
					pChar->NPC_OnHear("sell", m_pChar);
				break;

			case POPUP_PETGUARD:
				pChar->NPC_OnHearPetCmd("guard", m_pChar);
				break;

			case POPUP_PETFOLLOW:
				pChar->NPC_OnHearPetCmd("follow", m_pChar);
				break;

			case POPUP_PETDROP:
				pChar->NPC_OnHearPetCmd("drop", m_pChar);
				break;

			case POPUP_PETKILL:
				pChar->NPC_OnHearPetCmd("kill", m_pChar);
				break;

			case POPUP_PETSTOP:
				pChar->NPC_OnHearPetCmd("stop", m_pChar);
				break;

			case POPUP_PETSTAY:
				pChar->NPC_OnHearPetCmd("stay", m_pChar);
				break;

			case POPUP_PETFRIEND_ADD:
				pChar->NPC_OnHearPetCmd("friend", m_pChar);
				break;

			case POPUP_PETFRIEND_REMOVE:
				pChar->NPC_OnHearPetCmd("unfriend", m_pChar);
				break;

			case POPUP_PETTRANSFER:
				pChar->NPC_OnHearPetCmd("transfer", m_pChar);
				break;

			case POPUP_PETRELEASE:
				pChar->NPC_OnHearPetCmd("release", m_pChar);
				break;

			case POPUP_PETRENAME:
				if (pChar->NPC_IsOwnedBy(m_pChar))
					addPromptConsole(CLIMODE_PROMPT_NAME_PET, g_Cfg.GetDefaultMsg(DEFMSG_NPC_PET_RENAME_PROMPT), pChar->GetUID());
				break;

			case POPUP_STABLESTABLE:
				if ( pChar->m_pNPC->m_Brain == NPCBRAIN_STABLE )
					pChar->NPC_OnHear("stable", m_pChar);
				break;

			case POPUP_STABLERETRIEVE:
				if ( pChar->m_pNPC->m_Brain == NPCBRAIN_STABLE )
					pChar->NPC_OnHear("retrieve", m_pChar);
				break;

			case POPUP_TAME:
				if (m_pChar->Skill_CanUse(SKILL_TAMING) && !m_pChar->Skill_Wait(SKILL_TAMING))
				{
					m_pChar->m_Act_UID = pChar->GetUID();
					m_pChar->Skill_Start(SKILL_TAMING);
				}
				return;

		}

		if ((EntryTag >= POPUP_TRAINSKILL) && (EntryTag < POPUP_TRAINSKILL + g_Cfg.m_iMaxSkill))
		{
			tchar * pszMsg = Str_GetTemp();
			SKILL_TYPE iSkill = (SKILL_TYPE)(EntryTag - POPUP_TRAINSKILL);
			snprintf(pszMsg, Str_TempLength(), "train %s", g_Cfg.GetSkillKey(iSkill));
			pChar->NPC_OnHear(pszMsg, m_pChar);
			return;
		}
	}

	switch ( EntryTag )
	{
		case POPUP_PAPERDOLL:
			m_pChar->GetClientActive()->addCharPaperdoll(pChar);
			break;

		case POPUP_BACKPACK:
			m_pChar->Use_Obj(pChar->LayerFind(LAYER_PACK), false);
			break;

		case POPUP_PARTY_ADD:
			m_pChar->GetClientActive()->OnTarg_Party_Add(pChar);
			break;

		case POPUP_PARTY_REMOVE:
			if ( m_pChar->m_pParty )
				m_pChar->m_pParty->RemoveMember(pChar->GetUID(), m_pChar->GetUID());
			break;

		case POPUP_TRADE_ALLOW:
			m_pChar->SetDefNum("REFUSETRADES", 0);
			break;

		case POPUP_TRADE_REFUSE:
			m_pChar->SetDefNum("REFUSETRADES", 1);
			break;

		case POPUP_TRADE_OPEN:
			Cmd_SecureTrade(pChar, nullptr);
			break;

		case POPUP_GLOBALCHAT_ALLOW:
			if (m_pChar->m_pPlayer)
				m_pChar->m_pPlayer->m_fRefuseGlobalChatRequests = false;
			break;

		case POPUP_GLOBALCHAT_REFUSE:
			if (m_pChar->m_pPlayer)
				m_pChar->m_pPlayer->m_fRefuseGlobalChatRequests = true;
			break;
	}
}

void CClient::Event_BugReport( const tchar * pszText, int len, BUGREPORT_TYPE type, CLanguageID lang )
{
	ADDTOCALLSTACK("CClient::Event_BugReport");
	UnreferencedParameter(len);
	if ( !m_pChar )
		return;

	if ( IsTrigUsed(TRIGGER_USERBUGREPORT) )
	{
		CScriptTriggerArgs Args(type);
		Args.m_s1 = pszText;
		Args.m_VarsLocal.SetStr("LANG", false, lang.GetStr());

		m_pChar->OnTrigger(CTRIG_UserBugReport, m_pChar, &Args);
	}
}

void CClient::Event_UseToolbar(byte bType, dword dwArg)
{
	ADDTOCALLSTACK("CClient::Event_UseToolbar");
	if ( !m_pChar )
		return;

	if ( IsTrigUsed(TRIGGER_USERKRTOOLBAR) )
	{
		CScriptTriggerArgs Args( bType, dwArg );
		if ( m_pChar->OnTrigger( CTRIG_UserKRToolbar, m_pChar, &Args ) == TRIGRET_RET_TRUE )
			return;
	}

	switch (bType)
	{
		case 0x01: // Spell call
        if ( (SPELL_TYPE)dwArg <= SPELL_SPELLWEAVING_QTY )	// KR clients only have support up to spellweaving spells
        {
			Cmd_Skill_Magery((SPELL_TYPE)dwArg, m_pChar);
		}
        break;

		case 0x02: // Combat ability
        break;

		case 0x03: // Skill
			Event_Skill_Use((SKILL_TYPE)dwArg);
		    break;

		case 0x04: // Item
			Event_DoubleClick(CUID(dwArg), true, true);
            break;

        case 0x5:	// virtue
            Event_VirtueSelect(dwArg, m_pChar);
            return;
	}
}

//----------------------------------------------------------------------

void CClient::Event_ExtCmd( EXTCMD_TYPE type, tchar *pszName )
{
	ADDTOCALLSTACK("CClient::Event_ExtCmd");
	if ( !m_pChar )
		return;

    if (strnlen(pszName, MAX_EXTCMD_ARG_LEN) >= MAX_EXTCMD_ARG_LEN)
    {
        g_Log.EventWarn("%0x:Event_ExtCmd received too long argument\n", GetSocketID());
        return;
    }

    byte bDoorAutoDist = 1;
	if ( IsTrigUsed(TRIGGER_USEREXTCMD) )
	{
		CScriptTriggerArgs Args(pszName);
		Args.m_iN1 = type;

        if (type == EXTCMD_DOOR_AUTO)
            Args.m_VarsLocal.SetNumNew("DoorAutoDist", bDoorAutoDist);

		if ( m_pChar->OnTrigger(CTRIG_UserExtCmd, m_pChar, &Args) == TRIGRET_RET_TRUE )
			return;

        if (type == EXTCMD_DOOR_AUTO)
        {
            bDoorAutoDist = (byte)std::clamp(Args.m_VarsLocal.GetKeyNum("DoorAutoDist"), (int64)0, (int64)UO_MAP_VIEW_SIGHT);
        }

		Str_CopyLimitNull(pszName, Args.m_s1, MAX_TALK_BUFFER);
	}

	tchar *ppArgs[2];
	if (type != EXTCMD_DOOR_AUTO)
    {
        if ((*pszName == '\0') || (0 == Str_ParseCmds(pszName, ppArgs, ARRAY_COUNT(ppArgs), " ")))
        {
            g_Log.EventWarn("%0x:Event_ExtCmd received malformed data %d, '%s'\n", GetSocketID(), type, pszName);
            return;
        }
    }

	switch ( type )
	{
		case EXTCMD_OPEN_SPELLBOOK:	// open spell book if we have one.
		{
			CItem *pBook = nullptr;
			switch ( atoi(ppArgs[0]) )
			{
				default:
				case 1:	pBook = m_pChar->GetSpellbook(SPELL_Clumsy);				break;	// magery
				case 2:	pBook = m_pChar->GetSpellbook(SPELL_Animate_Dead_AOS);		break;	// necromancy
				case 3:	pBook = m_pChar->GetSpellbook(SPELL_Cleanse_by_Fire);		break;	// paladin
				case 4:	pBook = m_pChar->GetSpellbook(SPELL_Honorable_Execution);	break;	// bushido
				case 5:	pBook = m_pChar->GetSpellbook(SPELL_Focus_Attack);			break;	// ninjitsu
				case 6:	pBook = m_pChar->GetSpellbook(SPELL_Arcane_Circle);			break;	// spellweaving
				case 7:	pBook = m_pChar->GetSpellbook(SPELL_Nether_Bolt);			break;	// mysticism
				case 8:	pBook = m_pChar->GetSpellbook(SPELL_Inspire);				break;	// bard
			}
			if ( pBook )
				m_pChar->Use_Obj(pBook, true);
			return;
		}

		case EXTCMD_ANIMATE:
		{
			if ( !strnicmp(ppArgs[0], "bow", 3) )
				m_pChar->UpdateAnimate(ANIM_BOW);
			else if ( !strnicmp(ppArgs[0], "salute", 6) )
				m_pChar->UpdateAnimate(ANIM_SALUTE);
			else
				DEBUG_ERR(("%x:Event_ExtCmd Animate unk '%s'\n", GetSocketID(), ppArgs[0]));
			return;
		}

		case EXTCMD_SKILL:
		{
			Event_Skill_Use((SKILL_TYPE)(atoi(ppArgs[0])));
			return;
		}

		case EXTCMD_AUTOTARG:	// bizarre new autotarget mode. "target x y z"
		{
			CObjBase *pObj = CUID::ObjFindFromUID(atoi(ppArgs[0]));
			if ( pObj )
				DEBUG_ERR(("%x:Event_ExtCmd AutoTarg '%s' '%s'\n", GetSocketID(), pObj->GetName(), !ppArgs[1] ? TSTRING_NULL : ppArgs[1]));
			else
				DEBUG_ERR(("%x:Event_ExtCmd AutoTarg unk '%s' '%s'\n", GetSocketID(), ppArgs[0], !ppArgs[1] ? TSTRING_NULL : ppArgs[1]));
			return;
		}

		case EXTCMD_CAST_BOOK:	// cast spell from book.
		case EXTCMD_CAST_MACRO:	// macro spell.
		{
			SPELL_TYPE spell = (SPELL_TYPE)(atoi(ppArgs[0]));
			CSpellDef *pSpellDef = g_Cfg.GetSpellDef(spell);
			if ( !pSpellDef )
				return;

			if ( IsSetMagicFlags(MAGICF_PRECAST) && !pSpellDef->IsSpellType(SPELLFLAG_NOPRECAST) )
			{
				int skill;
				if ( !pSpellDef->GetPrimarySkill(&skill) )
					return;

				m_tmSkillMagery.m_iSpell = spell;
				m_pChar->m_atMagery.m_iSpell = spell;
				m_pChar->m_Act_p = m_pChar->GetTopPoint();
				m_pChar->m_Act_UID = m_Targ_UID;
				m_pChar->m_Act_Prv_UID = m_Targ_Prv_UID;
				m_pChar->Skill_Start((SKILL_TYPE)skill);
			}
			else
				Cmd_Skill_Magery(spell, m_pChar);
			return;
		}

		case EXTCMD_DOOR_AUTO:	// open door macro
		{
			CPointMap pt = m_pChar->GetTopPoint();
			char iCharZ = pt.m_z;

			pt.Move(m_pChar->m_dirFace);
            auto Area = CWorldSearchHolder::GetInstance(pt, bDoorAutoDist);
			for (;;)
			{
				CItem *pItem = Area->GetItem();
				if ( !pItem )
					return;

				switch ( pItem->GetType() )
				{
					case IT_DOOR:
					case IT_DOOR_LOCKED:
					case IT_PORTCULIS:
					case IT_PORT_LOCKED:
						if ( abs(iCharZ - pItem->GetTopPoint().m_z) < 20 )
						{
							m_pChar->SysMessageDefault(DEFMSG_MACRO_OPENDOOR);
							m_pChar->Use_Obj(pItem, true);
							return;
						}
				}
			}
			return;
		}

		case EXTCMD_INVOKE_VIRTUE:
		{
			if ( !IsTrigUsed(TRIGGER_USERVIRTUEINVOKE) )
				return;

			int iVirtueID = ppArgs[0][0] - '0';		// 0x1=Honor, 0x2=Sacrifice, 0x3=Valor
			CScriptTriggerArgs Args(m_pChar);
			Args.m_iN1 = iVirtueID;
			m_pChar->OnTrigger(CTRIG_UserVirtueInvoke, m_pChar, &Args);
			return;
		}

        /*
		default:
            // It can be a custom ext event
			g_Log.EventWarn("%x:Event_ExtCmd received unknown event type %d, '%s'\n", GetSocketID(), type, pszName);
			return;
        */
	}
}

// ---------------------------------------------------------------------

bool CClient::xPacketFilter( const byte * pData, uint iLen )
{
	ADDTOCALLSTACK("CClient::xPacketFilter");

	EXC_TRY("packet filter");
	if ( iLen > 0 && g_Serv.m_PacketFilter[pData[0]][0] )
	{
		CScriptTriggerArgs Args(pData[0]);
		enum TRIGRET_TYPE trigReturn;
		tchar idx[12];

		Args.m_s1 = GetPeerStr();
		Args.m_pO1 = this; // Yay for ARGO.SENDPACKET
		Args.m_VarsLocal.SetNum("CONNECTIONTYPE", GetConnectType());

        uint bytes = iLen;
        uint bytestr = minimum(bytes, SCRIPT_MAX_LINE_LEN);
		tchar *zBuf = Str_GetTemp();

		Args.m_VarsLocal.SetNum("NUM", bytes);
		memcpy(zBuf, &(pData[0]), bytestr);
		zBuf[bytestr] = 0;
		Args.m_VarsLocal.SetStr("STR", true, zBuf);
		if ( m_pAccount )
		{
			Args.m_VarsLocal.SetStr("ACCOUNT", false, m_pAccount->GetName());
			if ( m_pChar )
			{
				Args.m_VarsLocal.SetNum("CHAR", m_pChar->GetUID().GetObjUID());
			}
		}

		//	Fill locals [0..X] to the first X bytes of the packet
		for ( uint i = 0; i < bytes; ++i )
		{
			snprintf(idx, sizeof(idx), "%u", i);
			Args.m_VarsLocal.SetNum(idx, (int)(pData[i]));
		}

		//	Call the filtering function
		if ( g_Serv.r_Call(g_Serv.m_PacketFilter[pData[0]], &g_Serv, &Args, nullptr, &trigReturn) )
			if ( trigReturn == TRIGRET_RET_TRUE )
				return true;	// do not cry about errors
	}

	EXC_CATCH;
	return false;
}

bool CClient::xOutPacketFilter( const byte * pData, uint iLen )
{
	ADDTOCALLSTACK("CClient::xOutPacketFilter");

	EXC_TRY("Outgoing packet filter");
	if ( iLen > 0 && g_Serv.m_OutPacketFilter[pData[0]][0] )
	{
		CScriptTriggerArgs Args(pData[0]);
		enum TRIGRET_TYPE trigReturn;
		tchar idx[12];

		Args.m_s1 = GetPeerStr();
		Args.m_pO1 = this;
		Args.m_VarsLocal.SetNum("CONNECTIONTYPE", GetConnectType());

		size_t bytes = iLen;
		size_t bytestr = minimum(bytes, SCRIPT_MAX_LINE_LEN);
		tchar *zBuf = Str_GetTemp();

		Args.m_VarsLocal.SetNum("NUM", bytes);
		memcpy(zBuf, &(pData[0]), bytestr);
		zBuf[bytestr] = 0;
		Args.m_VarsLocal.SetStr("STR", true, zBuf);
		if ( m_pAccount )
		{
			Args.m_VarsLocal.SetStr("ACCOUNT", false, m_pAccount->GetName());
			if ( m_pChar )
			{
				Args.m_VarsLocal.SetNum("CHAR", m_pChar->GetUID().GetObjUID());
			}
		}

		//	Fill locals [0..X] to the first X bytes of the packet
		for ( size_t i = 0; i < bytes; ++i )
		{
			snprintf(idx, sizeof(idx), "%" PRIuSIZE_T, i);
			Args.m_VarsLocal.SetNum(idx, (int)(pData[i]));
		}

		//	Call the filtering function
		if ( g_Serv.r_Call(g_Serv.m_OutPacketFilter[pData[0]], &g_Serv, &Args, nullptr, &trigReturn) )
			if ( trigReturn == TRIGRET_RET_TRUE )
				return true;
	}

	EXC_CATCH;
	return false;
}
