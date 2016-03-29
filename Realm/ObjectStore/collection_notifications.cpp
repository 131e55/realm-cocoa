////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "collection_notifications.hpp"

#include "impl/background_collection.hpp"

#include <realm/link_view.hpp>
#include <realm/table_view.hpp>
#include <realm/util/assert.hpp>

using namespace realm;
using namespace realm::_impl;

NotificationToken::NotificationToken(std::shared_ptr<_impl::BackgroundCollection> query, size_t token)
: m_query(std::move(query)), m_token(token)
{
}

NotificationToken::~NotificationToken()
{
    // m_query itself (and not just the pointed-to thing) needs to be accessed
    // atomically to ensure that there are no data races when the token is
    // destroyed after being modified on a different thread.
    // This is needed despite the token not being thread-safe in general as
    // users find it very surpringing for obj-c objects to care about what
    // thread they are deallocated on.
    if (auto query = m_query.exchange({})) {
        query->remove_callback(m_token);
    }
}

NotificationToken::NotificationToken(NotificationToken&& rgt) = default;

NotificationToken& NotificationToken::operator=(realm::NotificationToken&& rgt)
{
    if (this != &rgt) {
        if (auto query = m_query.exchange({})) {
            query->remove_callback(m_token);
        }
        m_query = std::move(rgt.m_query);
        m_token = rgt.m_token;
    }
    return *this;
}

CollectionChangeIndices::CollectionChangeIndices(IndexSet deletions,
                                                 IndexSet insertions,
                                                 IndexSet modifications,
                                                 std::vector<Move> moves)
: deletions(std::move(deletions))
, insertions(std::move(insertions))
, modifications(std::move(modifications))
, moves(std::move(moves))
{
    for (auto&& move : this->moves) {
        this->deletions.add(move.from);
        this->insertions.add(move.to);
    }
}

void CollectionChangeBuilder::merge(CollectionChangeBuilder&& c)
{
    if (c.empty())
        return;
    if (empty()) {
        *this = std::move(c);
        return;
    }

    verify();
    c.verify();

    // First update any old moves
    if (!c.moves.empty() || !c.deletions.empty() || !c.insertions.empty()) {
        auto it = remove_if(begin(moves), end(moves), [&](auto& old) {
            // Check if the moved row was moved again, and if so just update the destination
            auto it = find_if(begin(c.moves), end(c.moves), [&](auto const& m) {
                return old.to == m.from;
            });
            if (it != c.moves.end()) {
                if (modifications.contains(it->from))
                    c.modifications.add(it->to);
                old.to = it->to;
                *it = c.moves.back();
                c.moves.pop_back();
                ++it;
                return false;
            }

            // Check if the destination was deleted
            // Removing the insert for this move will happen later
            if (c.deletions.contains(old.to))
                return true;

            // Update the destination to adjust for any new insertions and deletions
            old.to = c.insertions.shift(c.deletions.unshift(old.to));
            return false;
        });
        moves.erase(it, end(moves));
    }

    // Ignore new moves of rows which were previously inserted (the implicit
    // delete from the move will remove the insert)
    if (!insertions.empty() && !c.moves.empty()) {
        c.moves.erase(remove_if(begin(c.moves), end(c.moves),
                              [&](auto const& m) { return insertions.contains(m.from); }),
                    end(c.moves));
    }

    // Ensure that any previously modified rows which were moved are still modified
    if (!modifications.empty() && !c.moves.empty()) {
        for (auto const& move : c.moves) {
            if (modifications.contains(move.from))
                c.modifications.add(move.to);
        }
    }

    // Update the source position of new moves to compensate for the changes made
    // in the old changeset
    if (!deletions.empty() || !insertions.empty()) {
        for (auto& move : c.moves)
            move.from = deletions.shift(insertions.unshift(move.from));
    }

    moves.insert(end(moves), begin(c.moves), end(c.moves));

    // New deletion indices have been shifted by the insertions, so unshift them
    // before adding
    deletions.add_shifted_by(insertions, c.deletions);

    // Drop any inserted-then-deleted rows, then merge in new insertions
    insertions.erase_at(c.deletions);
    insertions.insert_at(c.insertions);

    clean_up_stale_moves();

    modifications.erase_at(c.deletions);
    modifications.shift_for_insert_at(c.insertions);
    modifications.add(c.modifications);

    c = {};
    verify();
}

void CollectionChangeBuilder::clean_up_stale_moves()
{
    // Look for moves which are now no-ops, and remove them plus the associated
    // insert+delete. Note that this isn't just checking for from == to due to
    // that rows can also be shifted by other inserts and deletes
    moves.erase(remove_if(begin(moves), end(moves), [&](auto const& move) {
        if (move.from - deletions.count(0, move.from) != move.to - insertions.count(0, move.to))
            return false;
        deletions.remove(move.from);
        insertions.remove(move.to);
        return true;
    }), end(moves));
}

void CollectionChangeBuilder::modify(size_t ndx)
{
    modifications.add(ndx);
}

void CollectionChangeBuilder::insert(size_t index, size_t count)
{
    modifications.shift_for_insert_at(index, count);
    insertions.insert_at(index, count);

    for (auto& move : moves) {
        if (move.to >= index)
            ++move.to;
    }
}

void CollectionChangeBuilder::erase(size_t index)
{
    modifications.erase_at(index);
    size_t unshifted = insertions.erase_or_unshift(index);
    if (unshifted != npos)
        deletions.add_shifted(unshifted);

    for (size_t i = 0; i < moves.size(); ++i) {
        auto& move = moves[i];
        if (move.to == index) {
            moves.erase(moves.begin() + i);
            --i;
        }
        else if (move.to > index)
            --move.to;
    }
}

void CollectionChangeBuilder::clear(size_t old_size)
{
    if (old_size != std::numeric_limits<size_t>::max()) {
        for (auto range : deletions)
            old_size += range.second - range.first;
        for (auto range : insertions)
            old_size -= range.second - range.first;
    }

    modifications.clear();
    insertions.clear();
    moves.clear();
    deletions.set(old_size);
}

void CollectionChangeBuilder::move(size_t from, size_t to)
{
    REALM_ASSERT(from != to);

    bool updated_existing_move = false;
    for (auto& move : moves) {
        if (move.to != from) {
            // Shift other moves if this row is moving from one side of them
            // to the other
            if (move.to >= to && move.to < from)
                ++move.to;
            else if (move.to <= to && move.to > from)
                --move.to;
            continue;
        }
        REALM_ASSERT(!updated_existing_move);

        // Collapse A -> B, B -> C into a single A -> C move
        move.to = to;
        updated_existing_move = true;

        insertions.erase_at(from);
        insertions.insert_at(to);
    }

    if (!updated_existing_move) {
        auto shifted_from = insertions.erase_or_unshift(from);
        insertions.insert_at(to);

        // Don't report deletions/moves for newly inserted rows
        if (shifted_from != npos) {
            shifted_from = deletions.add_shifted(shifted_from);
            moves.push_back({shifted_from, to});
        }
    }

    bool modified = modifications.contains(from);
    modifications.erase_at(from);

    if (modified)
        modifications.insert_at(to);
    else
        modifications.shift_for_insert_at(to);
}

void CollectionChangeBuilder::move_over(size_t row_ndx, size_t last_row)
{
    REALM_ASSERT(row_ndx <= last_row);
    REALM_ASSERT(insertions.empty() || prev(insertions.end())->second - 1 <= last_row);
    REALM_ASSERT(modifications.empty() || prev(modifications.end())->second - 1 <= last_row);
    if (row_ndx == last_row) {
        erase(row_ndx);
        return;
    }

    bool modified = modifications.contains(last_row);
    if (modified) {
        modifications.remove(last_row);
        modifications.add(row_ndx);
    }
    else
        modifications.remove(row_ndx);

    bool row_is_insertion = insertions.contains(row_ndx);
    bool last_is_insertion = !insertions.empty() && prev(insertions.end())->second == last_row + 1;
    REALM_ASSERT_DEBUG(insertions.empty() || prev(insertions.end())->second <= last_row + 1);

    bool updated_existing_move = false;
    if (row_is_insertion || last_is_insertion) {
        for (size_t i = 0; i < moves.size(); ++i) {
            auto& move = moves[i];
            // Remove moves to the row being deleted
            if (move.to == row_ndx) {
                moves.erase(moves.begin() + i);
                --i;
                continue;
            }
            if (move.to != last_row)
                continue;
            REALM_ASSERT(!updated_existing_move);

            // Collapse A -> B, B -> C into a single A -> C move
            move.to = row_ndx;
            updated_existing_move = true;
        }
    }

    // Don't report deletions/moves if last_row is newly inserted
    if (last_is_insertion) {
        insertions.remove(last_row);
    }
    // If it was previously moved, the unshifted source row has already been marked as deleted
    else if (!updated_existing_move) {
        auto shifted_last_row = insertions.unshift(last_row);
        shifted_last_row = deletions.add_shifted(shifted_last_row);
        moves.push_back({shifted_last_row, row_ndx});
    }

    // Don't mark the moved-over row as deleted if it was a new insertion
    if (!row_is_insertion) {
        deletions.add_shifted(insertions.unshift(row_ndx));
        insertions.add(row_ndx);
    }
    verify();
}

void CollectionChangeBuilder::verify()
{
#ifdef REALM_DEBUG
    for (auto&& move : moves) {
        REALM_ASSERT(deletions.contains(move.from));
        REALM_ASSERT(insertions.contains(move.to));
    }
#endif
}

namespace {
struct RowInfo {
    size_t row_index;
    size_t prev_tv_index;
    size_t tv_index;
    size_t shifted_tv_index;
};

void calculate_moves_unsorted(std::vector<RowInfo>& new_rows, IndexSet& removed, CollectionChangeIndices& changeset)
{
    size_t expected = 0;
    for (auto& row : new_rows) {
        // With unsorted queries rows only move due to move_last_over(), which
        // inherently can only move a row to earlier in the table.
        REALM_ASSERT(row.shifted_tv_index >= expected);
        if (row.shifted_tv_index == expected) {
            ++expected;
            continue;
        }

        // This row isn't just the row after the previous one, but it still may
        // not be a move if there were rows deleted between the two, so next
        // calcuate what row should be here taking those in to account
        size_t calc_expected = row.tv_index - changeset.insertions.count(0, row.tv_index) + removed.count(0, row.prev_tv_index);
        if (row.shifted_tv_index == calc_expected) {
            expected = calc_expected + 1;
            continue;
        }

        // The row still isn't the expected one, so it's a move
        changeset.moves.push_back({row.prev_tv_index, row.tv_index});
        changeset.insertions.add(row.tv_index);
        removed.add(row.prev_tv_index);
    }
}

class SortedMoveCalculator {
public:
    SortedMoveCalculator(std::vector<RowInfo>& new_rows, CollectionChangeIndices& changeset)
    : m_modified(changeset.modifications)
    {
        std::vector<Row> old_candidates;
        old_candidates.reserve(new_rows.size());
        for (auto& row : new_rows) {
            old_candidates.push_back({row.row_index, row.prev_tv_index});
        }
        std::sort(begin(old_candidates), end(old_candidates), [](auto a, auto b) {
            return std::tie(a.tv_index, a.row_index) < std::tie(b.tv_index, b.row_index);
        });

        // First check if the order of any of the rows actually changed
        size_t first_difference = npos;
        for (size_t i = 0; i < old_candidates.size(); ++i) {
            if (old_candidates[i].row_index != new_rows[i].row_index) {
                first_difference = i;
                break;
            }
        }
        if (first_difference == npos)
            return;

        // A map from row index -> tv index in new results
        b.reserve(new_rows.size());
        for (size_t i = 0; i < new_rows.size(); ++i)
            b.push_back({new_rows[i].row_index, i});
        std::sort(begin(b), end(b), [](auto a, auto b) {
            return std::tie(a.row_index, a.tv_index) < std::tie(b.row_index, b.tv_index);
        });

        a = std::move(old_candidates);

        find_longest_matches(first_difference, a.size(),
                             first_difference, new_rows.size());
        m_longest_matches.push_back({a.size(), new_rows.size(), 0});

        size_t i = first_difference, j = first_difference;
        for (auto match : m_longest_matches) {
            for (; i < match.i; ++i)
                changeset.deletions.add(a[i].tv_index);
            for (; j < match.j; ++j)
                changeset.insertions.add(new_rows[j].tv_index);
            i += match.size;
            j += match.size;
        }
    }

private:
    struct Match {
        size_t i, j, size, modified;
    };
    struct Row {
        size_t row_index;
        size_t tv_index;
    };

    IndexSet const& m_modified;
    std::vector<Match> m_longest_matches;

    std::vector<Row> a, b;

    Match find_longest_match(size_t begin1, size_t end1, size_t begin2, size_t end2)
    {
        struct Length {
            size_t j, len;
        };
        std::vector<Length> cur;
        std::vector<Length> prev;

        auto length = [&](size_t j) -> size_t {
            for (auto const& pair : prev) {
                if (pair.j + 1 == j)
                    return pair.len + 1;
            }
            return 1;
        };

        Match best = {begin1, begin2, 0, 0};

        for (size_t i = begin1; i < end1; ++i) {
            cur.clear();

            size_t ai = a[i].row_index;
            // Find the TV indicies at which this row appears in the new results
            // There should always be at least one (or it would have been filtered out earlier),
            // but can be multiple if there are dupes
            auto it = lower_bound(begin(b), end(b), Row{ai, 0},
                                  [](auto a, auto b) { return a.row_index < b.row_index; });
            REALM_ASSERT(it != end(b) && it->row_index == ai);
            for (; it != end(b) && it->row_index == ai; ++it) {
                size_t j = it->tv_index;
                if (j < begin2)
                    continue;
                if (j >= end2)
                    break; // b is sorted by tv_index so this can't transition from false to true

                size_t size = length(j);
                cur.push_back({j, size});
                if (size > best.size)
                    best = {i - size + 1, j - size + 1, size, npos};
                // Given two equal-length matches, prefer the one with fewer modified rows
                else if (size == best.size) {
                    if (best.modified == npos)
                        best.modified = m_modified.count(best.j - size + 1, best.j + 1);
                    auto count = m_modified.count(j - size + 1, j + 1);
                    if (count < best.modified)
                        best = {i - size + 1, j - size + 1, size, count};
                }
                REALM_ASSERT(best.i >= begin1 && best.i + best.size <= end1);
                REALM_ASSERT(best.j >= begin2 && best.j + best.size <= end2);
            }
            cur.swap(prev);
        }
        return best;
    }

    void find_longest_matches(size_t begin1, size_t end1, size_t begin2, size_t end2)
    {
        // FIXME: recursion could get too deep here
        // recursion depth worst case is currently O(N) and each recursion uses 320 bytes of stack
        // could reduce worst case to O(sqrt(N)) (and typical case to O(log N))
        // biasing equal selections towards the middle, but that's still
        // insufficient for Android's 8 KB stacks
        auto m = find_longest_match(begin1, end1, begin2, end2);
        if (!m.size)
            return;
        if (m.i > begin1 && m.j > begin2)
            find_longest_matches(begin1, m.i, begin2, m.j);
        m_longest_matches.push_back(m);
        if (m.i + m.size < end2 && m.j + m.size < end2)
            find_longest_matches(m.i + m.size, end1, m.j + m.size, end2);
    }
};

} // Anonymous namespace

CollectionChangeBuilder CollectionChangeBuilder::calculate(std::vector<size_t> const& prev_rows,
                                                           std::vector<size_t> const& next_rows,
                                                           std::function<bool (size_t)> row_did_change,
                                                           bool sort)
{
    REALM_ASSERT_DEBUG(sort || std::is_sorted(begin(next_rows), end(next_rows)));

    CollectionChangeBuilder ret;

    size_t deleted = 0;
    std::vector<RowInfo> old_rows;
    old_rows.reserve(prev_rows.size());
    for (size_t i = 0; i < prev_rows.size(); ++i) {
        if (prev_rows[i] == npos) {
            ++deleted;
            ret.deletions.add(i);
        }
        else
            old_rows.push_back({prev_rows[i], npos, i, i - deleted});
    }
    std::sort(begin(old_rows), end(old_rows), [](auto& lft, auto& rgt) {
        return lft.row_index < rgt.row_index;
    });

    std::vector<RowInfo> new_rows;
    new_rows.reserve(next_rows.size());
    for (size_t i = 0; i < next_rows.size(); ++i) {
        new_rows.push_back({next_rows[i], npos, i, 0});
    }
    std::sort(begin(new_rows), end(new_rows), [](auto& lft, auto& rgt) {
        return lft.row_index < rgt.row_index;
    });

    IndexSet removed;

    size_t i = 0, j = 0;
    while (i < old_rows.size() && j < new_rows.size()) {
        auto old_index = old_rows[i];
        auto new_index = new_rows[j];
        if (old_index.row_index == new_index.row_index) {
            new_rows[j].prev_tv_index = old_rows[i].tv_index;
            new_rows[j].shifted_tv_index = old_rows[i].shifted_tv_index;
            ++i;
            ++j;
        }
        else if (old_index.row_index < new_index.row_index) {
            removed.add(old_index.tv_index);
            ++i;
        }
        else {
            ret.insertions.add(new_index.tv_index);
            ++j;
        }
    }

    for (; i < old_rows.size(); ++i)
        removed.add(old_rows[i].tv_index);
    for (; j < new_rows.size(); ++j)
        ret.insertions.add(new_rows[j].tv_index);

    // Filter out the new insertions since we don't need them for any of the
    // further calculations
    new_rows.erase(std::remove_if(begin(new_rows), end(new_rows),
                                  [](auto& row) { return row.prev_tv_index == npos; }),
                   end(new_rows));
    std::sort(begin(new_rows), end(new_rows),
              [](auto& lft, auto& rgt) { return lft.tv_index < rgt.tv_index; });

    for (auto& row : new_rows) {
        if (row_did_change(row.row_index)) {
            ret.modifications.add(row.tv_index);
        }
    }

    if (sort) {
        SortedMoveCalculator(new_rows, ret);
    }
    else {
        calculate_moves_unsorted(new_rows, removed, ret);
    }
    ret.deletions.add(removed);
    ret.verify();

#ifdef REALM_DEBUG
    { // Verify that applying the calculated change to prev_rows actually produces next_rows
        auto rows = prev_rows;
        auto it = std::make_reverse_iterator(ret.deletions.end());
        auto end = std::make_reverse_iterator(ret.deletions.begin());
        for (; it != end; ++it) {
            rows.erase(rows.begin() + it->first, rows.begin() + it->second);
        }

        for (auto i : ret.insertions.as_indexes()) {
            rows.insert(rows.begin() + i, next_rows[i]);
        }

        REALM_ASSERT(rows == next_rows);
    }
#endif

    return ret;
}