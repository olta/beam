/*
	BmRefManager.h
		$Id$
*/

#ifndef _BmRefManager_h
#define _BmRefManager_h

#include <stdio.h>

#include <map>
#include <typeinfo>

#include <Autolock.h>
#include <Locker.h>
#include <String.h>

#include "BmBasics.h"
#include "BmLogHandler.h"


template <class T> class BmRef;
class BmProxy;
/*------------------------------------------------------------------------------*\
	BmRefObj
		-	an object that can be reference-managed
\*------------------------------------------------------------------------------*/
class BmRefObj {
	typedef map<BString,BmProxy*> BmProxyMap;

public:
	BmRefObj() : mRefCount(0) 				{}
	virtual ~BmRefObj() 						{}

	virtual const BString& RefName() const = 0;
	virtual const char* ProxyName() const
													{ return typeid(*this).name(); }

	// native methods:
	void AddRef();
	void RemoveRef();
	BString RefPrintHex() const;
	
	// getters:
	inline int32 RefCount() const			{ return mRefCount; }

	// statics:
	static BmProxy* GetProxy( const char* const proxyName);
#ifdef BM_REF_DEBUGGING
	static void PrintRefsLeft();
#endif

private:
	int32 mRefCount;
	static BmProxyMap nProxyMap;

	// Hide copy-constructor and assignment:
	BmRefObj( const BmRefObj&);
	BmRefObj operator=( const BmRefObj&);
};



typedef map<BString,BmRefObj*> BmObjectMap;
/*------------------------------------------------------------------------------*\
	BmProxy
		-	
\*------------------------------------------------------------------------------*/
class BmProxy {

public:
	inline BmProxy( BString name) : Locker(name.String()) {}
	BLocker Locker;
	BmObjectMap ObjectMap;
	BmRefObj* FetchObject( const BString& key);
};



/*------------------------------------------------------------------------------*\
	BmRef
		-	smart-pointer class that implements reference-counting (via BmRefObj)
\*------------------------------------------------------------------------------*/
template <class T> class BmRef {

	T* mPtr;

public:
	inline BmRef(T* p = 0) : mPtr( p) {
		AddRef( mPtr);
	}
	inline BmRef( const BmRef<T>& ref) : mPtr( ref.Get()) {
		AddRef( mPtr);
	}
	inline ~BmRef() {
		RemoveRef( mPtr);
	}
	inline BmRef<T>& operator= ( const BmRef<T>& ref) {
		if (mPtr != ref.Get()) {
			// in order to behave correctly when being called recursively,
			// we set new value before deleting old, so that a recursive call
			// will skip this block (because of the condition above).
			T* old = mPtr;
			mPtr = ref.Get();
			RemoveRef( old);
			AddRef( mPtr);
		}
		return *this;
	}
	inline BmRef<T>& operator= ( T* p) {
		if (mPtr != p) {
			// in order to behave correctly when being called recursively,
			// we set new value before deleting old, so that a recursive call
			// will skip this block (because of the condition above).
			T* old = mPtr;
			mPtr = p;
			RemoveRef( old);
			AddRef( mPtr);
		}
		return *this;
	}
	inline bool operator== ( const BmRef<T>& ref) {
		return mPtr == ref.Get();
	}
	inline bool operator== ( const T* p) {
		return mPtr == p;
	}
	inline bool operator!= ( const T* p) {
		return mPtr != p;
	}
	inline void Clear() {
		if (mPtr) {
			// in order to behave correctly when being called recursively,
			// we set new value before deleting old, so that a recursive call
			// will skip this block (because of the condition above).
			T* p = mPtr;
			mPtr = NULL;
			RemoveRef( p);
		}
	}
	inline T* operator->() const   		{ return mPtr; }
	inline T* Get() const 					{ return mPtr; }
	inline operator bool() const 			{ return mPtr!=NULL; }

private:
	inline void AddRef(T* p) const   	{ if (p)	p->AddRef(); }
	inline void RemoveRef(T* p) const 	{ if (p)	p->RemoveRef(); }
};



/*------------------------------------------------------------------------------*\
	BmWeakRef
		-	smart-pointer class that implements weak-referencing (via a set of BmRefObj)
		-	a weak reference is not included in reference-counting, but it transparently
			checks whether the weakly referenced object still exists or not.
\*------------------------------------------------------------------------------*/
template <class T> class BmWeakRef {

	BString mName;
	const char* mProxyName;

public:
	inline BmWeakRef(T* p = 0) 
	:	mName( p ? p->RefName() : "") 
	,	mProxyName( p ? p->ProxyName() : "") 
	{
		BM_LOG2( BM_LogUtil, BString("RefManager: weak-reference to <") << mName << "> created");
	}
	inline BmWeakRef<T>& operator= ( T* p) {
		mName = p ? p->RefName() : NULL;
		mProxyName = p ? p->ProxyName() : "";
		return *this;
	}
	inline bool operator== ( const T* p) {
		return p ? p->RefName() == mName : false;
	}
	inline bool operator!= ( const T* p) {
		return p ? p->RefName() != mName : true;
	}
	inline operator bool() const 			{ return Get(); }
	inline BmRef<T> Get() const 			{
		BM_LOG2( BM_LogUtil, BString("RefManager: weak-reference to <") << mName << "> dereferenced");
		BmProxy* proxy = BmRefObj::GetProxy( mProxyName);
		if (proxy) {
			BAutolock lock( &proxy->Locker);
			return static_cast<T*>(proxy->FetchObject( mName));
		} else 
			return NULL;
	}

private:
};



#endif
