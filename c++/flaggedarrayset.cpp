#include "flaggedarrayset.h"

#include <map>
#include <set>
#include <vector>
#include <list>
#include <thread>
#include <mutex>
#include <algorithm>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/******************************
 **** FlaggedArraySet util ****
 ******************************/
struct PtrPair {
	std::shared_ptr<std::vector<unsigned char> > elem;
	std::shared_ptr<std::vector<unsigned char> > elemHash;
	PtrPair(const std::shared_ptr<std::vector<unsigned char> >& elemIn, const std::shared_ptr<std::vector<unsigned char> >& elemHashIn) :
		elem(elemIn), elemHash(elemHashIn) {}
};

struct SharedPtrElem {
	PtrPair e;
	bool operator==(const SharedPtrElem& o) const { return *e.elemHash == *o.e.elemHash; }
	bool operator!=(const SharedPtrElem& o) const { return *e.elemHash != *o.e.elemHash; }
	bool operator< (const SharedPtrElem& o) const { return *e.elemHash <  *o.e.elemHash; }
	bool operator<=(const SharedPtrElem& o) const { return *e.elemHash <= *o.e.elemHash; }
	bool operator> (const SharedPtrElem& o) const { return *e.elemHash >  *o.e.elemHash; }
	bool operator>=(const SharedPtrElem& o) const { return *e.elemHash >= *o.e.elemHash; }
	SharedPtrElem(const PtrPair& eIn) : e(eIn) {}
};

class Deduper {
private:
	std::mutex dedup_mutex;
	std::set<FlaggedArraySet*> allArraySets;
	std::thread dedup_thread;
public:
	Deduper()
		: dedup_thread([&]() {
#ifdef PRECISE_BENCH
			return;
#endif
			while (true) {
				bool haveMultipleSets = false;
				{
					std::lock_guard<std::mutex> lock(dedup_mutex);
					haveMultipleSets = allArraySets.size() > 1;
				}

				if (haveMultipleSets) {
					std::list<PtrPair> ptrlist;

					{
						std::lock_guard<std::mutex> lock(dedup_mutex);
						for (FlaggedArraySet* fas : allArraySets) {
							if (fas->allowDups)
								continue;
							if (!fas->mutex.try_lock())
								continue;
							std::lock_guard<WaitCountMutex> lock(fas->mutex, std::adopt_lock);
							for (const auto& e : fas->backingMap) {
								if (fas->mutex.wait_count())
									break;
								assert(e.first.elem);
								ptrlist.push_back(PtrPair(e.first.elem, e.first.elemHash));
							}
						}
					}

					std::set<SharedPtrElem> txset;
					std::map<std::vector<unsigned char>*, PtrPair> duplicateMap;
					std::list<PtrPair> deallocList;
					for (const auto& ptr : ptrlist) {
						assert(ptr.elemHash);
						auto res = txset.insert(SharedPtrElem(ptr));
						if (!res.second && res.first->e.elem != ptr.elem)
							duplicateMap.insert(std::make_pair(&(*ptr.elem), res.first->e));
					}

					int dedups = 0;
					{
						std::lock_guard<std::mutex> lock(dedup_mutex);
						for (FlaggedArraySet* fas : allArraySets) {
							if (fas->allowDups)
								continue;
							if (!fas->mutex.try_lock())
								continue;
							std::lock_guard<WaitCountMutex> lock(fas->mutex, std::adopt_lock);
							for (auto& e : fas->backingMap) {
								if (fas->mutex.wait_count())
									break;
								assert(e.first.elem);
								auto it = duplicateMap.find(&(*e.first.elem));
								if (it != duplicateMap.end()) {
									assert(*it->second.elem == *e.first.elem);
									assert(*it->second.elemHash == *e.first.elemHash);
									deallocList.emplace_back(it->second);
									const_cast<ElemAndFlag&>(e.first).elem.swap(deallocList.back().elem);
									const_cast<ElemAndFlag&>(e.first).elemHash.swap(deallocList.back().elemHash);
									dedups++;
								}
							}
						}
					}
#ifdef FOR_TEST
					if (dedups)
						printf("Deduped %d txn\n", dedups);
#endif
				}
#ifdef FOR_TEST
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
#else
				std::this_thread::sleep_for(std::chrono::milliseconds(5000));
#endif
			}
		})
	{}

	~Deduper() {
		//TODO: close thread
	}

	void addFAS(FlaggedArraySet* fas) {
		std::lock_guard<std::mutex> lock(dedup_mutex);
		allArraySets.insert(fas);
	}

	void removeFAS(FlaggedArraySet* fas) {
		std::lock_guard<std::mutex> lock(dedup_mutex);
		allArraySets.erase(fas);
	}
};

static Deduper* deduper;

FlaggedArraySet::FlaggedArraySet(unsigned int maxSizeIn, bool allowDupsIn) :
		maxSize(maxSizeIn), backingMap(maxSize), allowDups(allowDupsIn) {
	backingMapBuckets = backingMap.bucket_count();
	partially_removed.reserve(maxSize);
	clear();
	if (!deduper)
		deduper = new Deduper();
	deduper->addFAS(this);
}

FlaggedArraySet::~FlaggedArraySet() {
	deduper->removeFAS(this);
	assert(sanity_check());
}


ElemAndFlag::ElemAndFlag(const std::shared_ptr<std::vector<unsigned char> >& elemIn, bool flagIn, bool allowDupsIn, bool setHash) :
	flag(flagIn), allowDups(allowDupsIn), elem(elemIn)
{
	if (setHash) {
		elemHash = std::make_shared<std::vector<unsigned char> >(32);
		double_sha256(&(*elem)[0], &(*elemHash)[0], elem->size());
	}
}
ElemAndFlag::ElemAndFlag(const std::vector<unsigned char>::const_iterator& elemBeginIn, const std::vector<unsigned char>::const_iterator& elemEndIn, bool flagIn, bool allowDupsIn) :
	flag(flagIn), allowDups(allowDupsIn), elemBegin(elemBeginIn), elemEnd(elemEndIn) {}

bool ElemAndFlag::operator == (const ElemAndFlag& o) const {
	if (elem && o.elem) {
		if (allowDups)
			return o.elem == elem;
		bool hashSet = o.elemHash && elemHash;
		return o.elem == elem ||
			(hashSet && *o.elemHash == *elemHash) ||
			(!hashSet && *o.elem == *elem);
	} else {
		std::vector<unsigned char>::const_iterator o_begin, o_end, e_begin, e_end;
		if (elem) {
			e_begin = elem->begin();
			e_end = elem->end();
		} else {
			e_begin = elemBegin;
			e_end = elemEnd;
		}
		if (o.elem) {
			o_begin = o.elem->begin();
			o_end = o.elem->end();
		} else {
			o_begin = o.elemBegin;
			o_end = o.elemEnd;
		}
		return o_end - o_begin == e_end - e_begin && !memcmp(&(*o_begin), &(*e_begin), o_end - o_begin);
	}
}

size_t std::hash<ElemAndFlag>::operator()(const ElemAndFlag& e) const {
	std::vector<unsigned char>::const_iterator it, end;
	if (e.elem) {
		it = e.elem->begin();
		end = e.elem->end();
	} else {
		it = e.elemBegin;
		end = e.elemEnd;
	}

	if (end - it < 5 + 32 + 4) {
		assert(0);
		return 42; // WAT?
	}
	it += 5 + 32 + 4 - 8;
	size_t res = 0;
	static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "Your size_t is neither 32-bit nor 64-bit?");
	for (unsigned int i = 0; i < 8; i += sizeof(size_t)) {
		for (unsigned int j = 0; j < sizeof(size_t); j++)
			res ^= *(it + i + j) << 8*j;
	}
	return res;
}


bool FlaggedArraySet::sanity_check() const {
	size_t size = indexMap.size();
	assert(backingMap.size() + partially_removed.size() == size);
	assert(this->size() == size - to_be_removed.size() - partially_removed.size());
	assert(backingMapBuckets == backingMap.bucket_count());

	size_t expected_flag_count = 0;

	size_t index = 0;
	for (size_t i = 0; i < size; i++) {
		std::unordered_map<ElemAndFlag, uint64_t>::iterator it = indexMap.at(i).first;
		if (indexMap.at(i).second) {
			assert(it != backingMap.end());
			assert(it->second == index + offset);
			assert(backingMap.find(it->first) == it);
			assert(&backingMap.find(it->first)->first == &it->first);
			if (it->first.flag)
				expected_flag_count++;
			index++;
		}
	}

	return flag_count == expected_flag_count;
}

void FlaggedArraySet::remove_(size_t index, bool partial) {
	assert(partial || partially_removed.empty());

	assert(index < indexMap.size());
	auto& rm = indexMap[index].first;
	assert(indexMap[index].second);
	if (rm->first.flag)
		flag_count--;

	size_t size = indexMap.size();

	if (index < size/2) {
		for (size_t i = 0; i < index; i++)
			if (indexMap[i].second)
				indexMap[i].first->second++;
		offset++;
	} else
		for (size_t i = index + 1; i < size; i++)
			if (indexMap[i].second)
				indexMap[i].first->second--;
	backingMap.erase(rm);

	if (!partial)
		indexMap.erase(indexMap.begin() + index);
	else {
		partially_removed.push_back(index);
		indexMap[index].second = false;
	}
}

inline void FlaggedArraySet::cleanup_late_remove() const {
	if (to_be_removed.size()) {
		for (unsigned int i = 0; i < to_be_removed.size(); i++) {
			assert((unsigned int)to_be_removed[i] < indexMap.size());
			const_cast<FlaggedArraySet*>(this)->remove_(to_be_removed[i], false);
		}
		to_be_removed.clear();
		max_remove = 0;
	}
	assert(sanity_check());
}

inline void FlaggedArraySet::cleanup_partially_removed() const {
	if (partially_removed.size()) {
		std::sort(partially_removed.begin(), partially_removed.end());
		for (ssize_t i = partially_removed.size() - 1; i >= 0; i--) {
			assert(!indexMap.at(partially_removed[i]).second);
			const_cast<FlaggedArraySet*>(this)->indexMap.erase(indexMap.begin() + partially_removed[i]);
		}
		partially_removed.clear();
	}
	assert(sanity_check());
}

inline void FlaggedArraySet::cleanup_all() const {
	cleanup_partially_removed();
	cleanup_late_remove();
}

bool FlaggedArraySet::contains(const std::shared_ptr<std::vector<unsigned char> >& e) const {
	std::lock_guard<WaitCountMutex> lock(mutex);
	cleanup_all();
	return backingMap.count(ElemAndFlag(e, false, allowDups, false));
}

void FlaggedArraySet::add(const std::shared_ptr<std::vector<unsigned char> >& e, bool flag) {
	ElemAndFlag elem(e, flag, allowDups, true);

	std::lock_guard<WaitCountMutex> lock(mutex);
	cleanup_all();

	auto res = backingMap.insert(std::make_pair(elem, size() + offset));
	if (!res.second)
		return;

	indexMap.push_back(std::make_pair(res.first, true));

	assert(size() <= maxSize + 1);
	while (size() > maxSize)
		remove_(0, false);

	if (flag)
		flag_count++;

	assert(sanity_check());
}

int FlaggedArraySet::remove(const std::vector<unsigned char>::const_iterator& start, const std::vector<unsigned char>::const_iterator& end) {
	std::lock_guard<WaitCountMutex> lock(mutex);
	cleanup_late_remove();

	auto it = backingMap.find(ElemAndFlag(start, end, false, allowDups));
	if (it == backingMap.end())
		return -1;

	int res = it->second - offset;
	size_t index = res;
	for (; index < indexMap.size(); index++)
		if (indexMap[index].second && indexMap[index].first == it)
			break;
	assert(index < indexMap.size() && indexMap[index].second && indexMap[index].first == it);

	remove_(index, true);
	assert(index < indexMap.size() && !indexMap[index].second);
	assert(backingMap.find(ElemAndFlag(std::make_shared<std::vector<unsigned char> >(start, end), false, allowDups, false)) == backingMap.end());

	assert(sanity_check());
	return res;
}

bool FlaggedArraySet::remove(int index, std::vector<unsigned char>& elemRes, unsigned char* elemHashRes) {
	std::lock_guard<WaitCountMutex> lock(mutex);

	cleanup_partially_removed();
	if (index < max_remove)
		cleanup_late_remove();
	int lookup_index = index + to_be_removed.size();

	if ((unsigned int)lookup_index >= indexMap.size())
		return false;

	assert(indexMap.at(lookup_index).second);
	const ElemAndFlag& e = indexMap[lookup_index].first->first;
	assert(e.elem && e.elemHash);
	memcpy(elemHashRes, &(*e.elemHash)[0], 32);
	elemRes = *e.elem;

	if (index >= max_remove) {
		to_be_removed.push_back(index);
		max_remove = index;
		if (e.flag) flags_to_remove++;
	} else {
		cleanup_late_remove();
		remove_(index, false);
	}

	assert(sanity_check());
	return true;
}

void FlaggedArraySet::clear() {
	std::lock_guard<WaitCountMutex> lock(mutex);
	if (!indexMap.empty() && !backingMap.empty())
		assert(sanity_check());

	flag_count = 0; offset = 0;
	flags_to_remove = 0; max_remove = 0;
	to_be_removed.clear(); partially_removed.clear();
	backingMap.clear(); indexMap.clear();
}

void FlaggedArraySet::for_all_txn(const std::function<void (const std::shared_ptr<std::vector<unsigned char> >&)> callback) const {
	std::lock_guard<WaitCountMutex> lock(mutex);
	cleanup_all();
	for (const auto& e : indexMap) {
		assert(e.first->first.elem);
		callback(e.first->first.elem);
	}
}
