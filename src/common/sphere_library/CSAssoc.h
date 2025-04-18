/*
* @file CSAssoc.h
* @brief Simple shared useful base classes.
*/

#ifndef _INC_CSASSOC_H
#define _INC_CSASSOC_H

#include "../sphere_library/CSString.h"
#include "CSObjList.h"


// -----------------------------
//	CValStr
// -----------------------------

struct CValStr
{
	// Associate a val with a string.
	// Assume sorted values from min to max.
	lpctstr m_pszName;
	int m_iVal;

public:
	lpctstr FindName(int iVal) const;
	void SetValues( int iVal, lpctstr pszName )
	{
		m_iVal = iVal;
		m_pszName = pszName;
	}
	void SetValue( int iVal )
	{
		m_iVal = iVal;
	}
};

enum ELEM_TYPE	// define types of structure/record elements.
{
	ELEM_VOID = 0,	// unknown what this might be. (or just 'other') (must be handled manually)
	ELEM_CSTRING,	// Size prefix.
	ELEM_BOOL,		// bool = just 1 byte i guess.
	ELEM_BYTE,		// 1 byte.
	ELEM_MASK_BYTE,	// bits in a byte
	ELEM_WORD,		// 2 bytes
	ELEM_MASK_WORD,	// bits in a word
	ELEM_INT,		// Whatever the int size is. 4 i assume
	ELEM_MASK_INT,
	ELEM_INT64,		// 8 bytes
	ELEM_MASK_INT64,// 8 bytes, hex
	ELEM_DWORD,		// 4 bytes.
	ELEM_MASK_DWORD,	// bits in a dword

	ELEM_QTY
};


struct CElementDef
{
	static const int sm_Lengths[ELEM_QTY];
	ELEM_TYPE m_type;
	size_t	m_offset;	// The offset into the class instance for this item.

	// get structure value.
	void * GetValPtr( const void * pBaseInst ) const;
	int GetValLength() const;

	bool GetValStr( const void * pBase, CSString & sVal ) const;
	bool SetValStr( void * pBase, lpctstr pszVal ) const;
};

struct CAssocReg	// associate members of some class/structure with entries in the registry.
{
	// LAST = { nullptr, 0, ELEM_VOID }
	static const char* m_sClassName;

	lpctstr m_pszKey;	// A single key identifier to be cat to a base key. nullptr=last
	CElementDef m_elem;

	operator lpctstr() const
	{
		return m_pszKey;
	}

	// get structure value.
	void * GetValPtr( const void * pBaseInst ) const
	{
		return m_elem.GetValPtr( pBaseInst );
	}
};

////////////////////////////////////////////////////////////////////////

class CSStringListRec : public CSObjListRec, public CSString
{
	friend class CSStringList;

public:
	static const char *m_sClassName;

    CSStringListRec( lpctstr pszVal );
    virtual ~CSStringListRec();

	CSStringListRec(const CSStringListRec& copy) = delete;
	CSStringListRec& operator=(const CSStringListRec& other) = delete;

	CSStringListRec * GetNext() const
	{
		return static_cast<CSStringListRec *>( CSObjListRec::GetNext() );
	}
};

class CSStringList : public CSObjList 	// obviously a list of strings.
{
public:
	static const char *m_sClassName;

	CSStringList();
	virtual ~CSStringList();

	CSStringList(const CSStringList& copy) = delete;
	CSStringList& operator=(const CSStringList& other) = delete;

	CSStringListRec * GetHead() const
	{
		return static_cast<CSStringListRec *>( CSObjList::GetContainerHead() );
	}
	void AddHead( lpctstr pszVal )
	{
		InsertContentHead( new CSStringListRec( pszVal ));
	}
	void AddTail( lpctstr pszVal )
	{
		InsertContentTail( new CSStringListRec( pszVal ));
	}
};

#endif // _INC_CSASSOC_H
