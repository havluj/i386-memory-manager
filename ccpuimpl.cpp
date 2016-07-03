#ifndef __PROGTEST__

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include "common.h"

using namespace std;
#endif /* __PROGTEST__ */

class CMemMngr {
public:
	CMemMngr(uint8_t* mem, uint32_t totalPages);
	~CMemMngr();

	uint32_t* allocatePage();
	void freePage(uint32_t pos);
	uint32_t getFreeSpace();
	uint32_t* setValue(uint32_t page, uint32_t offset, bool setVal, uint32_t val);

	uint8_t processCnt;
	pthread_mutex_t processCntLock;
	pthread_mutex_t memAcc;
	pthread_cond_t cond;
private:
	bool isUsed(uint32_t pos);
	void markUsed(uint32_t pos);
	void markUnused(uint32_t pos);

	uint32_t systemDataPos;
	uint8_t* memStart;
	uint32_t totalPages;

	uint32_t freeSpace;
	uint32_t memPointer;
};


CMemMngr::CMemMngr(uint8_t* memStart, uint32_t totalPages) : systemDataPos(0), memStart(memStart),
                                                             totalPages(totalPages)
{
	pthread_mutex_init(&processCntLock, NULL);
	pthread_mutex_init(&memAcc, NULL);
	pthread_cond_init(&cond, NULL);

	processCnt = 0;

	// how many pages do we need to store system info?
	uint32_t sysPageCnt = (totalPages / CCPU::PAGE_SIZE) + (totalPages % CCPU::PAGE_SIZE != 0);

	// set memory to 0 indicating that none of the pages are used
	memset(memStart, 0, CCPU::PAGE_SIZE * totalPages);

	// mark the first ones as used
	for(uint32_t i = 0; i < sysPageCnt; i++) {
		markUsed(i);
	}

	freeSpace = totalPages - sysPageCnt;
	memPointer = sysPageCnt;
}

CMemMngr::~CMemMngr()
{
	pthread_mutex_destroy(&processCntLock);
	pthread_mutex_destroy(&memAcc);
	pthread_cond_destroy(&cond);
}

bool CMemMngr::isUsed(uint32_t pos)
{
	uint32_t index = (pos) / 8;
	uint32_t offset = (pos) % 8;

	// we do not need to lock memAccess here because this function is called only from a already locked position
	// get the result (first, go to the right uint8_t number and then shift it to the right)
	uint8_t res = (memStart[index] >> (7 - offset)) & (uint8_t) 1;

	return res;
}

void CMemMngr::markUsed(uint32_t pos)
{
	uint32_t index = (pos) / 8;
	uint32_t offset = (pos) % 8;

	// we do not need to lock memAccess here because this function is called only from a already locked position
	memStart[index] |= (1 << (7 - offset));
}

void CMemMngr::markUnused(uint32_t pos)
{
	uint32_t index = (pos) / 8;
	uint32_t offset = (pos) % 8;

	// we do not need to lock memAccess here because this function is called only from a already locked position
	memStart[index] &= ~(1 << (7 - offset));
}

uint32_t* CMemMngr::allocatePage()
{
	// have we reached the end once already?
	bool roundTrip = false;

	while(isUsed(memPointer)) {
		if(memPointer >= totalPages) {
			if(roundTrip) { return NULL; }
			roundTrip = true;
			memPointer = 0;
			continue;
		}
		++memPointer;
	}

	markUsed(memPointer);
	memset(memStart + CCPU::PAGE_SIZE * memPointer, 0, CCPU::PAGE_SIZE);
	--freeSpace;

	return &memPointer;
}

void CMemMngr::freePage(uint32_t pos)
{
	++freeSpace;
	markUnused(pos);
}


uint32_t CMemMngr::getFreeSpace()
{
	return freeSpace;
}

uint32_t* CMemMngr::setValue(uint32_t page, uint32_t offset, bool setVal, uint32_t val)
{
	uint32_t* ptrLoc = (uint32_t*) ((memStart + CCPU::PAGE_SIZE * page) + (offset * 4));
	if(setVal) {
		*ptrLoc = val;
	}

	return ptrLoc;
}

//======================================================================================================================

class CCPUImpl : public CCPU {
public:
	CCPUImpl(uint8_t* memStart, uint32_t pageTableRoot, CMemMngr* memMngr);

	virtual uint32_t GetMemLimit(void) const;
	virtual bool SetMemLimit(uint32_t pages);
	virtual bool NewProcess(void* processArg, void (* entryPoint)(CCPU*, void*), bool copyMem);
protected:
	uint32_t memoryLimit;
	CMemMngr* memMngr;

	uint32_t lvl2PageTableRoot;
	uint32_t lvl1Offset;
	uint32_t lvl2Offset;

	/*
	 if copy-on-write is implemented:
	 virtual bool pageFaultHandler(uint32_t address, bool write);
	 */
};

//======================================================================================================================

void MemMgr(void* mem, uint32_t totalPages, void* processArg, void (* mainProcess)(CCPU*, void*))
{
	CMemMngr* memMngr = new CMemMngr((uint8_t*) mem, totalPages);

	pthread_mutex_lock(&memMngr->processCntLock);
	memMngr->processCnt++;
	pthread_mutex_unlock(&memMngr->processCntLock);

	CCPUImpl* ccpu = new CCPUImpl((uint8_t*) mem, *(memMngr->allocatePage()) << 12, memMngr);
	mainProcess(ccpu, processArg);

	pthread_mutex_lock(&memMngr->processCntLock);
	while(memMngr->processCnt > 1) {
		pthread_cond_wait(&memMngr->cond, &memMngr->processCntLock);
	}
	pthread_mutex_unlock(&memMngr->processCntLock);

	delete memMngr;
	delete ccpu;

	return;
}

struct processInfo {
	void* processArg;
	void (* entryPoint)(CCPU*, void*);
	bool copyMem;
	uint8_t* memStart;
	CMemMngr* memMngr;
	CCPU* oldCpu;
	uint32_t memoryLimit;

	bool finished = false;
	pthread_cond_t finishedCond;
	pthread_mutex_t finishedMutex;
};

void* processWrapper(void* atr)
{
	processInfo* info = (processInfo*) atr;

	CMemMngr* memMngr = info->memMngr;
	void* args = info->processArg;

	pthread_mutex_lock(&memMngr->memAcc);
	uint32_t pageTableRoot = *(info->memMngr->allocatePage()) << 12;
	pthread_mutex_unlock(&memMngr->memAcc);

	CCPUImpl* ccpu = new CCPUImpl(info->memStart, pageTableRoot, info->memMngr);
	if(info->copyMem) {
		ccpu->SetMemLimit(info->memoryLimit);
		uint32_t val;
		for(uint32_t addr = 0; addr < info->memoryLimit * CCPU::PAGE_SIZE; addr += 32) {
			info->oldCpu->ReadInt(addr, val);
			ccpu->WriteInt(addr, val);
		}
	}

	pthread_mutex_lock(&info->finishedMutex);
	info->finished = true;
	pthread_cond_signal(&info->finishedCond);
	pthread_mutex_unlock(&info->finishedMutex);

	// execute the process
	info->entryPoint(ccpu, args);

	// reduce the processCnt and send the signal if this is the last process
	pthread_mutex_lock(&memMngr->processCntLock);
	memMngr->processCnt--;
	if(memMngr->processCnt <= 1) {
		pthread_cond_signal(&memMngr->cond);
	}
	pthread_mutex_unlock(&memMngr->processCntLock);

	delete ccpu;

	return NULL;
}

//======================================================================================================================

CCPUImpl::CCPUImpl(uint8_t* memStart, uint32_t pageTableRoot, CMemMngr* memMngr) : CCPU(memStart, pageTableRoot),
                                                                                   memoryLimit(0), memMngr(memMngr),
                                                                                   lvl1Offset(0),
                                                                                   lvl2Offset(CCPU::PAGE_DIR_ENTRIES)
{ }

uint32_t CCPUImpl::GetMemLimit(void) const
{
	return memoryLimit;
}

bool CCPUImpl::SetMemLimit(uint32_t pages)
{
	uint32_t* ptrLoc;

	pthread_mutex_lock(&memMngr->memAcc);
	// are we allocating or freeing space?
	if(memoryLimit == pages) {
		pthread_mutex_unlock(&memMngr->memAcc);
		return true;
	} else if(memoryLimit > pages) {
		// free
		while(memoryLimit != pages) {
			if(lvl1Offset == 0) {
				// there is nothing more to remove
				pthread_mutex_unlock(&memMngr->memAcc);
				return true;
			}

			if(lvl2Offset == 0) {
				// free page
				if(lvl1Offset != 0) {
					memMngr->freePage(lvl2PageTableRoot);
				}

				// remove one entry from lvl1 pageTableRoot
				memMngr->setValue(m_PageTableRoot >> 12, --lvl1Offset, true, 0);

				// set lvl2 pageTableRoot for the previous entry
				if(lvl1Offset > 0) {
					ptrLoc = memMngr->setValue(m_PageTableRoot >> 12, lvl1Offset - 1, false, 0);
					lvl2PageTableRoot = (*ptrLoc) >> 12;
				} else {
					// there is nothing more to remove
					pthread_mutex_unlock(&memMngr->memAcc);
					return true;
				}

				lvl2Offset = CCPU::PAGE_DIR_ENTRIES;
			}

			if(lvl1Offset == 0) {
				// there is nothing more to remove
				pthread_mutex_unlock(&memMngr->memAcc);
				return true;
			}

			// free a single page
			ptrLoc = memMngr->setValue(lvl2PageTableRoot, --lvl2Offset, false, 0);
			memMngr->freePage((*ptrLoc) >> 12);
			memMngr->setValue(lvl2PageTableRoot, lvl2Offset, true, 0);

			memoryLimit--;
		}
	} else {
		if(memMngr->getFreeSpace() < (pages - memoryLimit)) {
			pthread_mutex_unlock(&memMngr->memAcc);
			return false;
		}

		// allocate
		while(pages != memoryLimit) {
			// is the lvl1 pageTableRoot full?
			if(lvl1Offset >= CCPU::PAGE_DIR_ENTRIES) {
				// this should be improved, but 1048576 pages (512 MiB) for one process is enough for now
				pthread_mutex_unlock(&memMngr->memAcc);
				return false;
			}

			// is the lvl2 pageTableRoot full?
			if(lvl2Offset == CCPU::PAGE_DIR_ENTRIES) {
				// create a new lvl2 pageTableRoot
				lvl2PageTableRoot = *(memMngr->allocatePage());
				memMngr->setValue(m_PageTableRoot >> 12, lvl1Offset, true, ((lvl2PageTableRoot << 12) | 0x00000FFF));

				++lvl1Offset;
				lvl2Offset = 0;
			}

			// create an entry in the lvl2 pageTableRoot
			memMngr->setValue(lvl2PageTableRoot, lvl2Offset, true, (*memMngr->allocatePage() << 12) | 0x00000FFF);

			++lvl2Offset;
			memoryLimit++;
		}
	}
	pthread_mutex_unlock(&memMngr->memAcc);

	return true;
}

bool CCPUImpl::NewProcess(void* processArg, void (* entryPoint)(CCPU*, void*), bool copyMem)
{
	if(copyMem && (memMngr->getFreeSpace() < memoryLimit)) {
		// there is not enough space to copy the memory
		return false;
	}

	pthread_mutex_lock(&memMngr->processCntLock);
	if(memMngr->processCnt >= PROCESS_MAX) {
		pthread_mutex_unlock(&memMngr->processCntLock);
		return false;
	}
	memMngr->processCnt++;
	pthread_mutex_unlock(&memMngr->processCntLock);

	processInfo* info = new processInfo;
	info->copyMem = copyMem;
	info->entryPoint = entryPoint;
	info->processArg = processArg;
	info->memMngr = memMngr;
	info->oldCpu = this;
	info->memStart = m_MemStart;
	info->memoryLimit = memoryLimit;

	int res;
	pthread_attr_t attr;
	pthread_t thread;

	res = pthread_attr_init(&attr);
	if(res != 0) {
		pthread_mutex_lock(&memMngr->processCntLock);
		memMngr->processCnt--;
		pthread_mutex_unlock(&memMngr->processCntLock);

		delete info;

		return false;
	}

	res = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if(res != 0) {
		pthread_mutex_lock(&memMngr->processCntLock);
		memMngr->processCnt--;
		pthread_mutex_unlock(&memMngr->processCntLock);

		pthread_attr_destroy(&attr);
		delete info;

		return false;
	}

	pthread_mutex_init(&info->finishedMutex, NULL);
	pthread_cond_init(&info->finishedCond, NULL);

	res = pthread_create(&thread, &attr, processWrapper, (void*) info);
	if(res != 0) {
		pthread_mutex_lock(&memMngr->processCntLock);
		memMngr->processCnt--;
		pthread_mutex_unlock(&memMngr->processCntLock);

		pthread_mutex_destroy(&info->finishedMutex);
		pthread_cond_destroy(&info->finishedCond);
		pthread_attr_destroy(&attr);
		delete info;

		return false;
	}

	pthread_attr_destroy(&attr);

	// wait for the other process to finish initializing
	// we need to copy all the data before this process dies
	pthread_mutex_lock(&info->finishedMutex);
	while(!info->finished) {
		pthread_cond_wait(&info->finishedCond, &info->finishedMutex);
	}
	pthread_mutex_unlock(&info->finishedMutex);
	// child process is now finished initializing, we can delete the temp structure
	pthread_mutex_destroy(&info->finishedMutex);
	pthread_cond_destroy(&info->finishedCond);
	delete info;

	return true;
}
