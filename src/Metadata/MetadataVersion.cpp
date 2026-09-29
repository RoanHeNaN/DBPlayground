#include "Metadata/IMetadataStore.h"
#include "fmt/format.h"

namespace dbplay {

std::string MetadataVersion::toString() const { return fmt::format("MetadataVersion{{opaque={}}}", opaque_); }

}  // namespace dbplay
