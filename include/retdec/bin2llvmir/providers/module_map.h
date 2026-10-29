/**
 * @file include/retdec/bin2llvmir/providers/module_map.h
 * @brief A per-module provider table that concurrent pipelines can share.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_MODULE_MAP_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_MODULE_MAP_H

#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace retdec {
namespace bin2llvmir {

/**
 * The providers keep one entry per @c llvm::Module in a process-wide table.
 * Each pipeline owns its module and is the only one that adds or erases that
 * module's entry, but several pipelines insert and erase at once, so every
 * operation on the tree itself takes a lock.
 *
 * What a lookup returns is safe to use after the lock is dropped: a
 * @c std::map node never moves, and only the owning pipeline erases it.
 */
template <typename Key, typename Value>
class ModuleMap
{
	public:
		using Map = std::map<Key, Value>;
		using iterator = typename Map::iterator;

		template <typename... Args>
		std::pair<iterator, bool> emplace(Args&&... args)
		{
			std::unique_lock<std::shared_mutex> l(_mutex);
			return _map.emplace(std::forward<Args>(args)...);
		}

		template <typename P>
		std::pair<iterator, bool> insert(P&& p)
		{
			std::unique_lock<std::shared_mutex> l(_mutex);
			return _map.insert(std::forward<P>(p));
		}

		iterator find(const Key& k)
		{
			std::shared_lock<std::shared_mutex> l(_mutex);
			return _map.find(k);
		}

		/// A std::map's end() is a sentinel that insertion and erasure do
		/// not move, so comparing a find() result against it is safe.
		iterator end()
		{
			return _map.end();
		}

		/// Drops one module's entry. The value is destroyed outside the lock.
		void erase(const Key& k)
		{
			Map dead;
			{
				std::unique_lock<std::shared_mutex> l(_mutex);
				auto it = _map.find(k);
				if (it != _map.end())
				{
					dead.insert(_map.extract(it));
				}
			}
		}

		/// Drops every module's entry. Only for single-pipeline callers
		/// (tests); a pipeline clears its own module with erase().
		void clear()
		{
			Map dead;
			{
				std::unique_lock<std::shared_mutex> l(_mutex);
				dead.swap(_map);
			}
		}

		std::size_t size() const
		{
			std::shared_lock<std::shared_mutex> l(_mutex);
			return _map.size();
		}

	private:
		mutable std::shared_mutex _mutex;
		Map _map;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
