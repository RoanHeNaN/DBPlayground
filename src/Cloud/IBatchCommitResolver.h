#ifndef DBPLAYGROUND_IBATCHCOMMITRESOLVER_H
#define DBPLAYGROUND_IBATCHCOMMITRESOLVER_H

#include <string>
#include <vector>

#include "Cloud/CloudTypes.h"
#include "Metadata/TableCurrentStateStore.h"

namespace dbplay {

enum class BatchCommitStatus { NotCommitted, Committed, Partial };

// Resolves client batch ids against the committed history represented by one
// CURRENT snapshot. A production implementation walks recent commit records
// and the compacted manifest's bounded dedupe index.
class IBatchCommitResolver {
 public:
  virtual ~IBatchCommitResolver() = default;
  virtual BatchCommitStatus Lookup(const TableDescriptor &table, const VersionedCurrentTableState &current_state,
                                   const std::vector<std::string> &batch_ids) const = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_IBATCHCOMMITRESOLVER_H
