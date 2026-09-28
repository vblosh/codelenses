#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "codelenses/domain/library.hpp"

namespace codelenses {

class Connection;

// Persistence for library profiles and their attachments to project workspaces.
class LibraryRepository {
public:
    explicit LibraryRepository(Connection& conn);

    int64_t create_profile(const LibraryProfile& profile);
    [[nodiscard]] std::vector<LibrarySourceRoot> list_source_roots(int64_t profile_id);
    [[nodiscard]] std::optional<LibraryProfile> get_profile(int64_t id);
    [[nodiscard]] std::optional<LibraryProfile> get_profile_by_workspace(int64_t workspace_id);
    [[nodiscard]] std::vector<LibraryProfile> list_profiles();
    bool update_profile(const LibraryProfile& profile);
    bool delete_profile(int64_t id);

    // Attachment lifecycle. Attaching is idempotent.
    bool attach(int64_t workspace_id, int64_t profile_id);
    bool detach(int64_t workspace_id, int64_t profile_id);
    [[nodiscard]] bool is_attached(int64_t workspace_id, int64_t profile_id);
    [[nodiscard]] std::vector<LibraryProfile> list_attached(int64_t workspace_id);
    [[nodiscard]] std::vector<int64_t> list_consumers(int64_t profile_id);

private:
    Connection& conn_;
};

} // namespace codelenses
