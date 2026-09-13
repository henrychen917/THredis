// acl.inc -- Redis 7.4 ACL machinery, textually included once by xshard.cc.

namespace tomo {
namespace {

constexpr uint32_t kAclMaxUsers = 65536;
std::mutex g_acl_mu;
std::array<std::atomic<AclUser*>, kAclMaxUsers> g_acl_slots{};
std::unordered_map<std::string, uint32_t> g_acl_names;
uint32_t g_acl_next_index = 1;
uint32_t g_acl_live_users = 0;
bool g_acl_initialized = false;
bool g_acl_pubsub_default_all = false;
Server* g_acl_server = nullptr;
std::string g_acl_filename;
const AclPerm g_acl_denied_permissions;
std::vector<const AclPerm*> g_acl_retired_permissions;

struct AclLogEntry {
    uint64_t count = 1;
    AclDeniedReason reason = AclDeniedReason::None;
    AclLogContext context = AclLogContext::Toplevel;
    std::string object;
    std::string username;
    int64_t updated_ms = 0;
    std::string client_info;
    uint64_t entry_id = 0;
    int64_t created_ms = 0;
};

struct AclLogStore {
    std::mutex mutex;
    std::deque<AclLogEntry> entries;
};

std::array<AclLogStore, kMaxThreads> g_acl_logs;
std::atomic<uint64_t> g_acl_log_max_len{128};
std::atomic<uint64_t> g_acl_log_next_id{0};

int64_t acl_wall_milliseconds() {
    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

struct AclUserImage {
    std::string name;
    AclPerm perm;
    std::vector<std::array<uint64_t, 4>> passwords;
    uint32_t flags = AclUserDisabled | AclUserSanitize;
};

bool acl_equal_icase(Slice value, const char* literal) {
    const size_t length = std::strlen(literal);
    if (value.n != length) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char left = static_cast<unsigned char>(value.p[i]);
        unsigned char right = static_cast<unsigned char>(literal[i]);
        if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right + ('a' - 'A'));
        if (left != right) return false;
    }
    return true;
}

std::string acl_lower(Slice value) {
    std::string result(value.p, value.n);
    for (char& byte : result)
        if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte + ('a' - 'A'));
    return result;
}

const AclCategoryDefinition* acl_category(Slice name) {
    for (const AclCategoryDefinition& category : kAclCategories)
        if (acl_equal_icase(name, category.name)) return &category;
    return nullptr;
}

bool acl_digest_equal(const std::array<uint64_t, 4>& left,
                      const std::array<uint64_t, 4>& right) {
    uint64_t difference = 0;
    for (uint32_t i = 0; i < left.size(); i++) difference |= left[i] ^ right[i];
    return difference == 0;
}

int acl_hex_nibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

bool acl_parse_hash(Slice value, std::array<uint64_t, 4>& digest) {
    if (value.n != 64) return false;
    uint8_t bytes[32];
    for (uint32_t i = 0; i < 32; i++) {
        const int high = acl_hex_nibble(value.p[i * 2]);
        const int low = acl_hex_nibble(value.p[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    std::memcpy(digest.data(), bytes, sizeof(bytes));
    return true;
}

std::string acl_hex_hash(const std::array<uint64_t, 4>& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    const auto* bytes = reinterpret_cast<const uint8_t*>(digest.data());
    std::string result(64, '0');
    for (uint32_t i = 0; i < 32; i++) {
        result[i * 2] = hex[bytes[i] >> 4];
        result[i * 2 + 1] = hex[bytes[i] & 15];
    }
    return result;
}

bool acl_command_bit(const AclSelector& perm, uint32_t id) {
    return perm.allowed_commands[id >> 6] & (uint64_t{1} << (id & 63));
}

void acl_set_command_bit(AclSelector& perm, uint32_t id, bool value) {
    if (id >= kAclFutureCommandBit) return;
    const uint64_t bit = uint64_t{1} << (id & 63);
    if (value) perm.allowed_commands[id >> 6] |= bit;
    else perm.allowed_commands[id >> 6] &= ~bit;
}

bool acl_future_commands(const AclSelector& perm) {
    return (perm.allowed_commands[kAclFutureCommandBit >> 6] &
            (uint64_t{1} << (kAclFutureCommandBit & 63))) != 0;
}

void acl_reset_commands(AclSelector& perm, bool all) {
    for (uint64_t& word : perm.allowed_commands) word = all ? UINT64_MAX : 0;
    if (all) perm.flags |= AclAllCommands;
    else perm.flags &= ~AclAllCommands;
    perm.command_rules.clear();
}

void acl_update_command_rule(AclSelector& perm, const std::string& rule) {
    const std::string target = acl_lower(
        Slice(rule.data() + 1, static_cast<uint32_t>(rule.size() - 1)));
    for (size_t i = 0; i < perm.command_rules.size();) {
        const std::string& old = perm.command_rules[i];
        if (old.size() > 1 && acl_lower(Slice(old.data() + 1,
                static_cast<uint32_t>(old.size() - 1))) == target)
            perm.command_rules.erase(perm.command_rules.begin() + i);
        else
            i++;
    }
    std::string normalized;
    normalized.push_back(rule[0]);
    normalized += target;
    perm.command_rules.push_back(std::move(normalized));
}

bool acl_apply_command_rule(AclSelector& perm, Slice rule, std::string& error,
                            bool remember = true) {
    if (rule.n < 2 || (rule.p[0] != '+' && rule.p[0] != '-')) {
        error = "Syntax error";
        return false;
    }
    if (std::memchr(rule.p + 1, '|', rule.n - 1)) {
        error = "Allowing first-arg of a subcommand is not supported";
        return false;
    }
    const bool allow = rule.p[0] == '+';
    const Slice target(rule.p + 1, rule.n - 1);
    if (target.n > 1 && target.p[0] == '@') {
        const Slice category_name(target.p + 1, target.n - 1);
        if (acl_equal_icase(category_name, "all")) {
            acl_reset_commands(perm, allow);
            return true;
        }
        const AclCategoryDefinition* category = acl_category(category_name);
        if (!category) {
            error = "Unknown command or category name in ACL";
            return false;
        }
        for (uint32_t id = 0; id < command_registry_size(); id++) {
            const CommandSpec* spec = command_registry_at(id);
            if (command_acl_category_mask(*spec) & category->bit)
                acl_set_command_bit(perm, id, allow);
        }
        if (!allow) perm.flags &= ~AclAllCommands;
    } else {
        const CommandSpec* spec = command_lookup(target);
        if (!spec) {
            error = "Unknown command or category name in ACL";
            return false;
        }
        acl_set_command_bit(perm, spec->id, allow);
        if (!allow) perm.flags &= ~AclAllCommands;
    }
    if (remember) acl_update_command_rule(perm, std::string(rule.p, rule.n));
    return true;
}

void acl_add_unique(std::vector<std::string>& patterns, Slice value) {
    for (const std::string& old : patterns)
        if (old.size() == value.n && !std::memcmp(old.data(), value.p, value.n)) return;
    patterns.emplace_back(value.p, value.n);
}

bool acl_valid_username(Slice name) {
    if (!name.n) return false;
    for (uint32_t i = 0; i < name.n; i++)
        if (name.p[i] == '\0' || name.p[i] == ' ' || name.p[i] == '\t' ||
            name.p[i] == '\r' || name.p[i] == '\n') return false;
    return true;
}

AclUserImage acl_new_image(const std::string& name) {
    AclUserImage image;
    image.name = name;
    image.flags = AclUserDisabled | AclUserSanitize;
    if (g_acl_pubsub_default_all) image.perm.flags |= AclAllChannels;
    return image;
}

bool acl_apply_permission_rule(AclSelector& perm, Slice rule, std::string& error) {
    if (acl_equal_icase(rule, "allkeys") || acl_equal_icase(rule, "~*")) {
        perm.flags |= AclAllKeys;
        perm.key_patterns.clear();
    } else if (acl_equal_icase(rule, "resetkeys")) {
        perm.flags &= ~AclAllKeys;
        perm.key_patterns.clear();
    } else if (acl_equal_icase(rule, "allchannels") || acl_equal_icase(rule, "&*")) {
        perm.flags |= AclAllChannels;
        perm.channel_patterns.clear();
    } else if (acl_equal_icase(rule, "resetchannels")) {
        perm.flags &= ~AclAllChannels;
        perm.channel_patterns.clear();
    } else if (acl_equal_icase(rule, "allcommands")) {
        acl_reset_commands(perm, true);
    } else if (acl_equal_icase(rule, "nocommands")) {
        acl_reset_commands(perm, false);
    } else if (rule.p[0] == '%') {
        error = "Read/write key patterns are not supported until command key specifications are available";
        return false;
    } else if (rule.p[0] == '~') {
        if (perm.flags & AclAllKeys) {
            error = "Adding a pattern after the * pattern (or the 'allkeys' flag) is not valid and does not have any effect. Try 'resetkeys' to start with an empty list of patterns";
            return false;
        }
        acl_add_unique(perm.key_patterns, Slice(rule.p + 1, rule.n - 1));
    } else if (rule.p[0] == '&') {
        if (perm.flags & AclAllChannels) {
            error = "Adding a pattern after the * pattern (or the 'allchannels' flag) is not valid and does not have any effect. Try 'resetchannels' to start with an empty list of channels";
            return false;
        }
        acl_add_unique(perm.channel_patterns, Slice(rule.p + 1, rule.n - 1));
    } else if (!acl_apply_command_rule(perm, rule, error)) {
        return false;
    }
    return true;
}

AclUserImage acl_image(const AclUser& user) {
    AclUserImage image;
    image.name = user.name;
    image.flags = user.flags;
    image.passwords = user.passwords;
    const AclPerm* perm = user.perm.load(std::memory_order_acquire);
    if (perm) image.perm = *perm;
    return image;
}

bool acl_apply_rule(AclUserImage& image, Slice rule, std::string& error) {
    if (!rule.n) return true;
    if (acl_equal_icase(rule, "on")) {
        image.flags |= AclUserEnabled;
        image.flags &= ~AclUserDisabled;
    } else if (acl_equal_icase(rule, "off")) {
        image.flags |= AclUserDisabled;
        image.flags &= ~AclUserEnabled;
    } else if (acl_equal_icase(rule, "sanitize-payload")) {
        image.flags |= AclUserSanitize;
        image.flags &= ~AclUserSkipSanitize;
    } else if (acl_equal_icase(rule, "skip-sanitize-payload")) {
        image.flags |= AclUserSkipSanitize;
        image.flags &= ~AclUserSanitize;
    } else if (acl_equal_icase(rule, "nopass")) {
        image.flags |= AclUserNoPass;
        image.passwords.clear();
    } else if (acl_equal_icase(rule, "resetpass")) {
        image.flags &= ~AclUserNoPass;
        image.passwords.clear();
    } else if (acl_equal_icase(rule, "reset")) {
        image.passwords.clear();
        image.flags = AclUserDisabled | AclUserSanitize;
        image.perm = AclPerm{};
        if (g_acl_pubsub_default_all) image.perm.flags |= AclAllChannels;
    } else if (acl_equal_icase(rule, "clearselectors")) {
        image.perm.selectors.clear();
    } else if (rule.p[0] == '>' || rule.p[0] == '#') {
        std::array<uint64_t, 4> digest;
        if (rule.p[0] == '>') digest = auth_password_digest(Slice(rule.p + 1, rule.n - 1));
        else if (!acl_parse_hash(Slice(rule.p + 1, rule.n - 1), digest)) {
            error = "The password hash must be exactly 64 characters and contain only lowercase hexadecimal characters";
            return false;
        }
        bool duplicate = false;
        for (const auto& password : image.passwords) duplicate |= acl_digest_equal(password, digest);
        if (!duplicate) image.passwords.push_back(digest);
        image.flags &= ~AclUserNoPass;
    } else if (rule.p[0] == '<' || rule.p[0] == '!') {
        std::array<uint64_t, 4> digest;
        if (rule.p[0] == '<') digest = auth_password_digest(Slice(rule.p + 1, rule.n - 1));
        else if (!acl_parse_hash(Slice(rule.p + 1, rule.n - 1), digest)) {
            error = "The password hash must be exactly 64 characters and contain only lowercase hexadecimal characters";
            return false;
        }
        auto found = std::find_if(image.passwords.begin(), image.passwords.end(),
            [&](const auto& password) { return acl_digest_equal(password, digest); });
        if (found == image.passwords.end()) {
            error = "The password you are trying to remove from the user does not exist";
            return false;
        }
        image.passwords.erase(found);
    } else if (rule.p[0] == '(' || rule.p[rule.n - 1] == ')') {
        error = "Syntax error";
        return false;
    } else if (!acl_apply_permission_rule(image.perm, rule, error)) {
        return false;
    }
    return true;
}

bool acl_apply_selector(AclUserImage& image, const std::string& expression,
                        std::string& error) {
    AclSelector selector;
    if (g_acl_pubsub_default_all) selector.flags |= AclAllChannels;
    const char* text = expression.data() + 1;
    const char* const end = expression.data() + expression.size() - 1;
    while (text < end) {
        while (text < end && std::isspace(static_cast<unsigned char>(*text))) text++;
        const char* token = text;
        while (text < end && !std::isspace(static_cast<unsigned char>(*text))) text++;
        if (token == text) break;
        const Slice rule(token, static_cast<uint32_t>(text - token));
        if (!acl_apply_permission_rule(selector, rule, error)) return false;
    }
    if (!(selector.flags & AclAllCommands)) {
        for (uint32_t id = 0; id < command_registry_size(); id++) {
            const CommandSpec* spec = command_registry_at(id);
            if ((spec->flags & CmdFlags::ScriptRoute) && acl_command_bit(selector, id)) {
                error = "Script commands in ACL selectors require allcommands";
                return false;
            }
        }
    }
    image.perm.selectors.push_back(std::move(selector));
    return true;
}

bool acl_apply_rules(AclUserImage& image, const std::vector<std::string>& rules,
                     size_t begin, std::string& error, std::string* failed = nullptr) {
    for (size_t i = begin; i < rules.size(); i++) {
        if (!rules[i].empty() && rules[i][0] == '(') {
            std::string expression = rules[i];
            const std::string selector_start = expression;
            while (expression.empty() || expression.back() != ')') {
                if (++i == rules.size()) {
                    error = "Unmatched parenthesis in acl selector starting at '" +
                            selector_start + "'.";
                    if (failed) *failed = expression;
                    return false;
                }
                expression.push_back(' ');
                expression += rules[i];
            }
            if (!acl_apply_selector(image, expression, error)) {
                if (failed) *failed = expression;
                return false;
            }
            continue;
        }
        const Slice rule(rules[i].data(), static_cast<uint32_t>(rules[i].size()));
        if (!acl_apply_rule(image, rule, error)) {
            if (failed) *failed = rules[i];
            return false;
        }
    }
    return true;
}

void acl_verify_command_description(const AclSelector& perm) {
    AclSelector predicted;
    acl_reset_commands(predicted, acl_future_commands(perm));
    for (const std::string& rule : perm.command_rules) {
        std::string error;
        if (!acl_apply_command_rule(predicted,
                Slice(rule.data(), static_cast<uint32_t>(rule.size())), error, false)) std::abort();
    }
    if (std::memcmp(predicted.allowed_commands, perm.allowed_commands,
                    sizeof(perm.allowed_commands))) std::abort();
}

std::string acl_describe_commands(const AclSelector& perm) {
    acl_verify_command_description(perm);
    std::string result = acl_future_commands(perm) ? "+@all" : "-@all";
    for (const std::string& rule : perm.command_rules) {
        result.push_back(' ');
        result += rule;
    }
    return result;
}

std::string acl_join_patterns(const AclSelector& perm, bool channel) {
    std::string result;
    if (channel) {
        if (perm.flags & AclAllChannels) return "&*";
        result = "resetchannels";
        for (const std::string& pattern : perm.channel_patterns) {
            result += " &";
            result += pattern;
        }
    } else {
        if (perm.flags & AclAllKeys) return "~*";
        for (const std::string& pattern : perm.key_patterns) {
            if (!result.empty()) result.push_back(' ');
            result.push_back('~');
            result += pattern;
        }
    }
    return result;
}

std::string acl_getuser_channels(const AclSelector& perm) {
    if (perm.flags & AclAllChannels) return "&*";
    std::string result;
    for (const std::string& pattern : perm.channel_patterns) {
        if (!result.empty()) result.push_back(' ');
        result.push_back('&');
        result += pattern;
    }
    return result;
}

std::string acl_describe_image(const AclUserImage& image) {
    std::string result;
    auto flag = [&](const char* text) {
        if (!result.empty()) result.push_back(' ');
        result += text;
    };
    if (image.flags & AclUserEnabled) flag("on");
    if (image.flags & AclUserDisabled) flag("off");
    if (image.flags & AclUserNoPass) flag("nopass");
    if (image.flags & AclUserSkipSanitize) flag("skip-sanitize-payload");
    if (image.flags & AclUserSanitize) flag("sanitize-payload");
    for (const auto& password : image.passwords) flag(("#" + acl_hex_hash(password)).c_str());
    const std::string keys = acl_join_patterns(image.perm, false);
    if (!keys.empty()) flag(keys.c_str());
    flag(acl_join_patterns(image.perm, true).c_str());
    flag(acl_describe_commands(image.perm).c_str());
    for (const AclSelector& selector : image.perm.selectors) {
        std::string description = "(";
        const std::string keys = acl_join_patterns(selector, false);
        if (!keys.empty()) description += keys + " ";
        description += acl_join_patterns(selector, true);
        description.push_back(' ');
        description += acl_describe_commands(selector);
        description.push_back(')');
        flag(description.c_str());
    }
    return result;
}

AclUser* acl_slot(uint32_t index) {
    if (index >= kAclMaxUsers) return nullptr;
    return g_acl_slots[index].load(std::memory_order_acquire);
}

bool acl_default_permission_unrestricted_locked() {
    AclUser* user = acl_slot(kAclDefaultUser);
    if (!user || !(user->flags & AclUserEnabled)) return false;
    const AclPerm* perm = user->perm.load(std::memory_order_acquire);
    return perm && (perm->flags & (AclAllKeys | AclAllCommands | AclAllChannels)) ==
                       (AclAllKeys | AclAllCommands | AclAllChannels);
}

void acl_refresh_security_locked() {
    if (!g_acl_server) return;
    AclUser* user = acl_slot(kAclDefaultUser);
    bool required = true;
    if (user) {
        required = !(user->flags & AclUserEnabled) || !(user->flags & AclUserNoPass);
    }
    g_acl_server->set_auth_config(required);
    g_acl_server->set_acl_active(g_acl_live_users > 1 ||
                                 !acl_default_permission_unrestricted_locked());
}

bool acl_publish_image_locked(AclUser* user, AclUserImage&& image) {
    auto* perm = new (std::nothrow) AclPerm(std::move(image.perm));
    if (!perm) return false;
    const AclPerm* retired = user->perm.exchange(perm, std::memory_order_acq_rel);
    user->passwords = std::move(image.passwords);
    user->flags = image.flags;
    if (retired) {
        g_acl_retired_permissions.push_back(retired);
        if (g_acl_server) g_acl_server->acl_perm_retired();
    }
    return true;
}

void acl_tombstone_user_locked(uint32_t index) {
    AclUser* user = acl_slot(index);
    if (!user) return;
    const AclPerm* retired = user->perm.exchange(
        &g_acl_denied_permissions, std::memory_order_acq_rel);
    user->passwords.clear();
    user->flags = AclUserDisabled | AclUserSanitize;
    if (retired) {
        g_acl_retired_permissions.push_back(retired);
        if (g_acl_server) g_acl_server->acl_perm_retired();
    }
}

AclUser* acl_create_locked(AclUserImage&& image, uint32_t requested_index = UINT32_MAX) {
    const uint32_t index = requested_index == UINT32_MAX ? g_acl_next_index++ : requested_index;
    if (index >= kAclMaxUsers || g_acl_names.count(image.name)) return nullptr;
    auto* user = new (std::nothrow) AclUser;
    if (!user) return nullptr;
    user->name = image.name;
    user->index = index;
    if (!acl_publish_image_locked(user, std::move(image))) {
        delete user;
        return nullptr;
    }
    g_acl_names.emplace(user->name, index);
    g_acl_slots[index].store(user, std::memory_order_release);
    g_acl_live_users++;
    return user;
}

bool acl_pattern_allowed(const AclSelector& perm, Slice value) {
    for (const std::string& pattern : perm.key_patterns)
        if (command_glob_match(Slice(pattern.data(), static_cast<uint32_t>(pattern.size())), value))
            return true;
    return false;
}

bool acl_channel_allowed(const AclSelector& perm, Slice value, bool literal_pattern) {
    for (const std::string& pattern : perm.channel_patterns) {
        const Slice allowed(pattern.data(), static_cast<uint32_t>(pattern.size()));
        if (literal_pattern ? (allowed.n == value.n && !std::memcmp(allowed.p, value.p, value.n))
                            : command_glob_match(allowed, value)) return true;
    }
    return false;
}

bool acl_command_allowed(const AclSelector& perm, const CommandSpec& spec) {
    if (!(perm.flags & AclAllCommands) && !acl_command_bit(perm, spec.id)) return false;
    // The current script engine can only prove nested-command admission for an unrestricted
    // command set. Keep that existing safety boundary independently for root and each selector.
    return !(spec.flags & CmdFlags::ScriptRoute) || (perm.flags & AclAllCommands);
}

template <typename GetArg>
AclDeniedReason acl_check_keys(const AclSelector& perm, const CommandSpec& spec, uint32_t argc,
                               GetArg&& get_arg, uint32_t* denied_arg) {
    if (perm.flags & AclAllKeys) return AclDeniedReason::None;
    const Slice command(spec.name, static_cast<uint32_t>(std::strlen(spec.name)));
    if (command.eq_icase("sort")) {
        for (uint32_t arg = 2; arg < argc; arg++) {
            if (get_arg(arg).eq_icase("by") || get_arg(arg).eq_icase("get")) {
                if (denied_arg) *denied_arg = arg;
                return AclDeniedReason::Key;
            }
            if (get_arg(arg).eq_icase("store") && arg + 1 < argc &&
                !acl_pattern_allowed(perm, get_arg(arg + 1))) {
                if (denied_arg) *denied_arg = arg + 1;
                return AclDeniedReason::Key;
            }
        }
    }
    if (spec.flags & CmdFlags::ScriptRoute) {
        if (argc < 3) return AclDeniedReason::None;
        uint64_t count = 0;
        const Slice text = get_arg(2);
        if (!text.n) return AclDeniedReason::None;
        for (uint32_t i = 0; i < text.n; i++) {
            if (text.p[i] < '0' || text.p[i] > '9') return AclDeniedReason::None;
            count = count * 10 + static_cast<uint64_t>(text.p[i] - '0');
        }
        for (uint32_t arg = 3; arg < argc && arg < 3 + count; arg++)
            if (!acl_pattern_allowed(perm, get_arg(arg))) {
                if (denied_arg) *denied_arg = arg;
                return AclDeniedReason::Key;
            }
        return AclDeniedReason::None;
    }
    if (spec.flags & CmdFlags::StreamRoute) {
        uint32_t streams = 1;
        while (streams < argc && !get_arg(streams).eq_icase("streams")) streams++;
        if (streams == argc || ((argc - streams - 1) & 1u)) return AclDeniedReason::None;
        const uint32_t count = (argc - streams - 1) / 2;
        for (uint32_t arg = streams + 1; arg < streams + 1 + count; arg++)
            if (!acl_pattern_allowed(perm, get_arg(arg))) {
                if (denied_arg) *denied_arg = arg;
                return AclDeniedReason::Key;
            }
        return AclDeniedReason::None;
    }
    if (spec.first_key <= 0 || spec.key_step <= 0 || argc == 0)
        return AclDeniedReason::None;
    const uint32_t last = spec.last_key < 0 ? argc - 1 :
        std::min<uint32_t>(static_cast<uint32_t>(spec.last_key), argc - 1);
    for (uint32_t arg = static_cast<uint32_t>(spec.first_key); arg <= last;
         arg += static_cast<uint32_t>(spec.key_step))
        if (!acl_pattern_allowed(perm, get_arg(arg))) {
            if (denied_arg) *denied_arg = arg;
            return AclDeniedReason::Key;
        }
    return AclDeniedReason::None;
}

template <typename GetArg>
__attribute__((noinline)) AclDeniedReason acl_check_permission(
        uint32_t user_index, const CommandSpec& spec, uint32_t argc,
        GetArg&& get_arg, uint32_t* denied_arg = nullptr) {
    if (spec.flags & CmdFlags::AclExempt) return AclDeniedReason::None;
    AclUser* user = g_acl_slots[user_index].load(std::memory_order_acquire);
    if (!user) return AclDeniedReason::Command;
    const AclPerm* permissions = user->perm.load(std::memory_order_acquire);
    if (!permissions) return AclDeniedReason::Command;

    bool command_allowed = false;
    uint32_t first_denied_arg = UINT32_MAX;
    auto check = [&](const AclSelector& selector) {
        if (!acl_command_allowed(selector, spec)) return false;
        command_allowed = true;
        uint32_t candidate = UINT32_MAX;
        const AclDeniedReason denied = acl_check_keys(
            selector, spec, argc, get_arg, &candidate);
        if (denied == AclDeniedReason::None) return true;
        if (first_denied_arg == UINT32_MAX) first_denied_arg = candidate;
        return false;
    };
    if (check(*permissions)) return AclDeniedReason::None;
    for (const AclSelector& selector : permissions->selectors)
        if (check(selector)) return AclDeniedReason::None;
    if (!command_allowed) return AclDeniedReason::Command;
    if (denied_arg) *denied_arg = first_denied_arg;
    return AclDeniedReason::Key;
}

void acl_reply_denied(Client& client, Op& op, uint32_t user_index, AclDeniedReason reason,
                      uint32_t denied_arg, ThreadCtx& thread,
                      AclLogContext context = AclLogContext::Toplevel) {
    LoopSignals& signals = thread.sig();
    const char* username_text = acl_username(user_index);
    const Slice username(username_text, static_cast<uint32_t>(std::strlen(username_text)));
    Slice object = op.cmd_name();
    std::string normalized_command;
    if (reason == AclDeniedReason::Command) {
        normalized_command = acl_lower(op.cmd_name());
        object = Slice(normalized_command.data(), static_cast<uint32_t>(normalized_command.size()));
    }
    if ((reason == AclDeniedReason::Key || reason == AclDeniedReason::Channel) &&
        denied_arg < op.argc()) object = op.arg(denied_arg);
    acl_log_denial(thread, client, reason, context, object, username);
    if (reason == AclDeniedReason::Command) {
        signals.acl_access_denied_cmd++;
        std::string message = "NOPERM User ";
        message += username_text;
        message += " has no permissions to run the '";
        message += acl_lower(op.cmd_name());
        message += "' command";
        reply_err(op.sink(), message.c_str());
    } else if (reason == AclDeniedReason::Key) {
        signals.acl_access_denied_key++;
        if (op.cmd_name().eq_icase("sort")) {
            for (uint32_t arg = 2; arg < op.argc(); arg++) {
                if (op.arg(arg).eq_icase("by")) {
                    reply_err(op.sink(), "ERR BY option of SORT denied due to insufficient ACL permissions.");
                    return;
                }
                if (op.arg(arg).eq_icase("get")) {
                    reply_err(op.sink(), "ERR GET option of SORT denied due to insufficient ACL permissions.");
                    return;
                }
            }
        }
        reply_err(op.sink(), "NOPERM No permissions to access a key");
    } else if (reason == AclDeniedReason::Channel) {
        signals.acl_access_denied_channel++;
        reply_err(op.sink(), "NOPERM No permissions to access a channel");
    }
}

bool acl_parse_file(const std::string& filename, std::vector<AclUserImage>& images,
                    std::string& errors) {
    std::FILE* file = std::fopen(filename.c_str(), "r");
    if (!file) {
        errors = "Error loading ACLs, opening file '" + filename + "': " + std::strerror(errno);
        return false;
    }
    std::string contents;
    char chunk[4096];
    while (const size_t count = std::fread(chunk, 1, sizeof(chunk), file))
        contents.append(chunk, count);
    if (std::ferror(file)) errors += "Error loading ACLs, reading file '" + filename + "'. ";
    std::fclose(file);
    uint32_t lineno = 0;
    std::unordered_set<std::string> names;
    size_t position = 0;
    while (position < contents.size()) {
        lineno++;
        const size_t newline = contents.find('\n', position);
        const size_t end = newline == std::string::npos ? contents.size() : newline;
        std::string line = contents.substr(position, end - position);
        position = newline == std::string::npos ? contents.size() : newline + 1;
        const char* first = line.c_str();
        while (*first && std::isspace(static_cast<unsigned char>(*first))) first++;
        if (!*first || *first == '#') continue;
        std::vector<std::string> words;
        if (!cfg_split_args(first, words)) {
            errors += filename + ":" + std::to_string(lineno) + ": unbalanced quotes in acl line. ";
            continue;
        }
        if (words.empty()) continue;
        if (words[0] != "user" || words.size() < 2) {
            errors += filename + ":" + std::to_string(lineno) +
                      " should start with user keyword followed by the username. ";
            continue;
        }
        if (!acl_valid_username(Slice(words[1].data(), words[1].size()))) {
            errors += "'" + filename + ":" + std::to_string(lineno) + ": username '" +
                      words[1] + "' contains invalid characters. ";
            continue;
        }
        if (!names.insert(words[1]).second) {
            errors += "WARNING: Duplicate user '" + words[1] + "' found on line " +
                      std::to_string(lineno) + ". ";
            continue;
        }
        AclUserImage image = acl_new_image(words[1]);
        std::string error, failed;
        if (!acl_apply_rules(image, words, 2, error, &failed)) {
            if (error == "Unknown command or category name in ACL")
                errors += filename + ":" + std::to_string(lineno) +
                          ": Error in applying operation '" + failed + "': " + error + ". ";
            else
                errors += filename + ":" + std::to_string(lineno) + ": " + error + ". ";
            continue;
        }
        images.push_back(std::move(image));
    }
    if (!errors.empty()) {
        errors += "WARNING: ACL errors detected, no change to the previously active ACL rules was performed";
        return false;
    }
    bool has_default = false;
    for (const auto& image : images) has_default |= image.name == "default";
    if (!has_default) {
        AclUserImage image = acl_new_image("default");
        image.flags = AclUserEnabled | AclUserNoPass | AclUserSanitize;
        image.perm.flags = AclAllKeys | AclAllChannels;
        acl_reset_commands(image.perm, true);
        images.push_back(std::move(image));
    }
    return true;
}

bool acl_commit_images_locked(std::vector<AclUserImage>& images, IoLoop* loop = nullptr) {
    std::unordered_set<std::string> incoming;
    for (const auto& image : images) incoming.insert(image.name);
    for (auto it = g_acl_names.begin(); it != g_acl_names.end();) {
        if (it->first != "default" && !incoming.count(it->first)) {
            if (loop) acl_broadcast_user_change(*loop, it->second, nullptr, true);
            acl_tombstone_user_locked(it->second);
            g_acl_live_users--;
            it = g_acl_names.erase(it);
        } else {
            ++it;
        }
    }
    for (AclUserImage& image : images) {
        auto found = g_acl_names.find(image.name);
        if (found == g_acl_names.end()) {
            if (!acl_create_locked(std::move(image))) return false;
        } else {
            AclUser* user = acl_slot(found->second);
            if (!user) return false;
            if (!acl_publish_image_locked(user, std::move(image))) return false;
            if (loop)
                acl_broadcast_user_change(
                    *loop, user->index, user->perm.load(std::memory_order_acquire), false);
        }
    }
    acl_refresh_security_locked();
    return true;
}

std::vector<AclUser*> acl_sorted_users_locked() {
    std::vector<AclUser*> users;
    users.reserve(g_acl_names.size());
    for (const auto& entry : g_acl_names) {
        AclUser* user = acl_slot(entry.second);
        if (user) users.push_back(user);
    }
    std::sort(users.begin(), users.end(), [](const AclUser* left, const AclUser* right) {
        return left->name < right->name;
    });
    return users;
}

bool acl_save_file_locked() {
    std::string contents;
    for (AclUser* user : acl_sorted_users_locked()) {
        contents += "user ";
        contents += user->name;
        contents.push_back(' ');
        contents += acl_describe_image(acl_image(*user));
        contents.push_back('\n');
    }
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    const std::string temporary = g_acl_filename + ".tmp-" + std::to_string(::getpid()) + "-" +
                                  std::to_string(ts.tv_sec * 1000ll + ts.tv_nsec / 1000000);
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(temporary.c_str(), g_acl_filename.c_str()) != 0) ok = false;
    if (ok) {
        const size_t slash = g_acl_filename.rfind('/');
        const std::string directory = slash == std::string::npos ? "." :
                                      (slash == 0 ? "/" : g_acl_filename.substr(0, slash));
        const int dirfd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        if (dirfd < 0 || ::fsync(dirfd) != 0) ok = false;
        if (dirfd >= 0) ::close(dirfd);
    }
    if (!ok) ::unlink(temporary.c_str());
    return ok;
}

void acl_reply_category(Op& op) {
    if (op.argc() == 2) {
        reply_array_header(op.sink(), kAclCategoryCount);
        for (const AclCategoryDefinition& category : kAclCategories)
            reply_bulk(op.sink(), Slice(category.name, std::strlen(category.name)));
        return;
    }
    const AclCategoryDefinition* category = acl_category(op.arg(2));
    if (!category) {
        const size_t length = std::min<size_t>(op.arg(2).n, 128);
        std::string message = "ERR Unknown category '";
        message.append(op.arg(2).p, length);
        message.push_back('\'');
        reply_err(op.sink(), message.c_str());
        return;
    }
    std::vector<const CommandMetadata*> commands;
    for (uint32_t id = 0; id < command_metadata_size(); id++) {
        const CommandMetadata* spec = command_metadata_at(id);
        if (command_metadata_categories(*spec) & category->bit) commands.push_back(spec);
    }
    std::sort(commands.begin(), commands.end(), [](const CommandMetadata* left,
                                                    const CommandMetadata* right) {
        const Slice left_name = command_metadata_name(*left);
        const Slice right_name = command_metadata_name(*right);
        const size_t common = std::min(left_name.n, right_name.n);
        const int order = std::memcmp(left_name.p, right_name.p, common);
        return order ? order < 0 : left_name.n < right_name.n;
    });
    reply_array_header(op.sink(), commands.size());
    for (const CommandMetadata* spec : commands)
        reply_bulk(op.sink(), command_metadata_name(*spec));
}

const char* kAclNoFileError =
    "ERR This Redis instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG REWRITE (assuming you have a Redis configuration file set) in order to store users in the Redis configuration.";

const char* acl_log_reason_name(AclDeniedReason reason) {
    switch (reason) {
        case AclDeniedReason::Command: return "command";
        case AclDeniedReason::Key: return "key";
        case AclDeniedReason::Channel: return "channel";
        case AclDeniedReason::Auth: return "auth";
        default: return "unknown";
    }
}

const char* acl_log_context_name(AclLogContext context) {
    switch (context) {
        case AclLogContext::Toplevel: return "toplevel";
        case AclLogContext::Multi: return "multi";
        case AclLogContext::Lua: return "lua";
        case AclLogContext::Module: return "module";
    }
    return "unknown";
}

void acl_log_reset() {
    for (AclLogStore& store : g_acl_logs) {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.entries.clear();
    }
}

bool acl_parse_log_count(Slice value, int64_t& count) {
    if (!value.n || value.n > 20) return false;
    uint32_t position = 0;
    bool negative = false;
    if (value.p[0] == '-') { negative = true; position = 1; }
    if (position == value.n) return false;
    // ACL LOG count has the same string2ll grammar as SLOWLOG GET: canonical decimal only.
    if (value.p[position] == '0') {
        if (negative || position + 1 != value.n) return false;
        count = 0;
        return true;
    }
    if (value.p[position] < '1' || value.p[position] > '9') return false;
    uint64_t parsed = 0;
    for (; position < value.n; position++) {
        const char byte = value.p[position];
        if (byte < '0' || byte > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
    if (parsed > limit) return false;
    count = negative ? (parsed == (uint64_t{1} << 63) ? INT64_MIN
                                                       : -static_cast<int64_t>(parsed))
                     : static_cast<int64_t>(parsed);
    return true;
}

void acl_reply_log(Op& op) {
    if (op.argc() == 3 && op.arg(2).eq_icase("reset")) {
        acl_log_reset();
        reply_ok(op.sink());
        return;
    }
    int64_t requested = 10;
    if (op.argc() == 3 && !acl_parse_log_count(op.arg(2), requested)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    if (requested < 0) requested = 0;
    std::vector<AclLogEntry> entries;
    for (AclLogStore& store : g_acl_logs) {
        std::lock_guard<std::mutex> lock(store.mutex);
        entries.insert(entries.end(), store.entries.begin(), store.entries.end());
    }
    std::sort(entries.begin(), entries.end(), [](const AclLogEntry& left,
                                                  const AclLogEntry& right) {
        if (left.updated_ms != right.updated_ms) return left.updated_ms > right.updated_ms;
        return left.entry_id > right.entry_id;
    });
    if (static_cast<uint64_t>(requested) < entries.size())
        entries.resize(static_cast<size_t>(requested));
    reply_array_header(op.sink(), entries.size());
    const int64_t now = acl_wall_milliseconds();
    for (const AclLogEntry& entry : entries) {
        auto sink = op.sink();
        reply_array_header(sink, 20);  // RESP2 encoding of vanilla's ten-field map
        reply_bulk(sink, Slice("count", 5));
        reply_int(sink, static_cast<long long>(entry.count));
        reply_bulk(sink, Slice("reason", 6));
        const char* reason = acl_log_reason_name(entry.reason);
        reply_bulk(sink, Slice(reason, std::strlen(reason)));
        reply_bulk(sink, Slice("context", 7));
        const char* context = acl_log_context_name(entry.context);
        reply_bulk(sink, Slice(context, std::strlen(context)));
        reply_bulk(sink, Slice("object", 6));
        reply_bulk(sink, Slice(entry.object.data(), entry.object.size()));
        reply_bulk(sink, Slice("username", 8));
        reply_bulk(sink, Slice(entry.username.data(), entry.username.size()));
        reply_bulk(sink, Slice("age-seconds", 11));
        reply_double(sink, std::max<int64_t>(0, now - entry.updated_ms) / 1000.0);
        reply_bulk(sink, Slice("client-info", 11));
        reply_bulk(sink, Slice(entry.client_info.data(), entry.client_info.size()));
        reply_bulk(sink, Slice("entry-id", 8));
        reply_int(sink, static_cast<long long>(entry.entry_id));
        reply_bulk(sink, Slice("timestamp-created", 17));
        reply_int(sink, static_cast<long long>(entry.created_ms));
        reply_bulk(sink, Slice("timestamp-last-updated", 22));
        reply_int(sink, static_cast<long long>(entry.updated_ms));
    }
}

}  // namespace

void acl_set_log_max_len(uint64_t max_len) {
    g_acl_log_max_len.store(max_len, std::memory_order_release);
    for (AclLogStore& store : g_acl_logs) {
        std::lock_guard<std::mutex> lock(store.mutex);
        while (store.entries.size() > max_len) store.entries.pop_back();
    }
}

void acl_log_denial(ThreadCtx& thread, const Client& client, AclDeniedReason reason,
                    AclLogContext context, Slice object, Slice username) {
    uint64_t max_len = g_acl_log_max_len.load(std::memory_order_acquire);
    if (!max_len || thread.id() >= g_acl_logs.size()) return;
    const int64_t now = acl_wall_milliseconds();
    AclLogEntry candidate;
    candidate.reason = reason;
    candidate.context = context;
    candidate.object.assign(object.p, object.n);
    candidate.username.assign(username.p, username.n);
    candidate.updated_ms = now;
    candidate.created_ms = now;
    candidate.client_info = "id=" + std::to_string(client.id()) +
        " addr=unknown:0 name= age=0 idle=0 flags=N db=0 sub=0 psub=0 ssub=0";

    AclLogStore& store = g_acl_logs[thread.id()];
    std::lock_guard<std::mutex> lock(store.mutex);
    max_len = g_acl_log_max_len.load(std::memory_order_acquire);
    if (!max_len) return;
    const size_t scan = std::min<size_t>(10, store.entries.size());
    for (size_t i = 0; i < scan; i++) {
        AclLogEntry& old = store.entries[i];
        if (old.reason != reason || old.context != context || old.object != candidate.object ||
            old.username != candidate.username || now - old.updated_ms > 60000) continue;
        old.count++;
        old.updated_ms = now;
        old.client_info = std::move(candidate.client_info);
        if (i) {
            AclLogEntry updated = std::move(old);
            store.entries.erase(store.entries.begin() + static_cast<ptrdiff_t>(i));
            store.entries.push_front(std::move(updated));
        }
        return;
    }
    candidate.entry_id = g_acl_log_next_id.fetch_add(1, std::memory_order_relaxed);
    store.entries.push_front(std::move(candidate));
    while (store.entries.size() > max_len) store.entries.pop_back();
}

void acl_broadcast_user_change(IoLoop& loop, uint32_t user_index,
                               const AclPerm* permissions, bool deleted) {
    loop.acl_broadcast_user_change(user_index, permissions, deleted);
}

bool acl_initialize(Server& server, const Config& config, std::string& error) {
    std::lock_guard<std::mutex> lock(g_acl_mu);
    if (g_acl_initialized) return true;
    g_acl_server = &server;
    g_acl_filename = config.aclfile ? config.aclfile : "";
    g_acl_pubsub_default_all = config.acl_pubsub_allchannels != 0;
    g_acl_log_max_len.store(config.acllog_max_len, std::memory_order_relaxed);
    AclUserImage default_image = acl_new_image("default");
    default_image.flags = AclUserEnabled | AclUserNoPass | AclUserSanitize;
    default_image.perm.flags = AclAllKeys | AclAllChannels;
    acl_reset_commands(default_image.perm, true);
    if (config.requirepass && *config.requirepass) {
        default_image.flags &= ~AclUserNoPass;
        default_image.passwords.push_back(
            auth_password_digest(Slice(config.requirepass, std::strlen(config.requirepass))));
    }
    if (!acl_create_locked(std::move(default_image), kAclDefaultUser)) {
        error = "ACL initialization failed: out of memory";
        return false;
    }
    g_acl_initialized = true;

    if (!g_acl_filename.empty()) {
        std::vector<AclUserImage> images;
        if (!acl_parse_file(g_acl_filename, images, error) || !acl_commit_images_locked(images)) {
            if (error.empty()) error = "ACL initialization failed: out of memory";
            return false;
        }
    } else if (!config.acl_users.empty()) {
        std::unordered_set<std::string> names;
        std::vector<AclUserImage> images;
        for (const auto& definition : config.acl_users) {
            if (definition.empty()) continue;
            if (!names.insert(definition[0]).second) {
                error = "Duplicate user found. A user can only be defined once in config files";
                return false;
            }
            AclUserImage image = acl_new_image(definition[0]);
            std::string rule_error, failed;
            if (!acl_apply_rules(image, definition, 1, rule_error, &failed)) {
                error = "Error in ACL SETUSER modifier '" + failed + "': " + rule_error;
                return false;
            }
            images.push_back(std::move(image));
        }
        bool has_default = false;
        for (const auto& image : images) has_default |= image.name == "default";
        if (!has_default) images.push_back(acl_image(*acl_slot(kAclDefaultUser)));
        if (!acl_commit_images_locked(images)) {
            error = "ACL initialization failed: out of memory";
            return false;
        }
    } else {
        acl_refresh_security_locked();
    }
    return true;
}

void acl_shutdown() {
    std::lock_guard<std::mutex> lock(g_acl_mu);
    if (!g_acl_initialized) return;
    // Published permissions are process-lifetime RCU objects while IO/executor readers are live.
    // Shutdown runs after every worker joined, which is the first universal grace period and the
    // only safe point at which both retired and current images can be reclaimed without an IO ack.
    for (const AclPerm* permissions : g_acl_retired_permissions) delete permissions;
    g_acl_retired_permissions.clear();
    for (uint32_t index = 0; index < g_acl_next_index; index++) {
        AclUser* user = g_acl_slots[index].exchange(nullptr, std::memory_order_acq_rel);
        if (!user) continue;
        const AclPerm* permissions = user->perm.load(std::memory_order_relaxed);
        if (permissions && permissions != &g_acl_denied_permissions) delete permissions;
        delete user;
    }
    g_acl_names.clear();
    g_acl_next_index = 1;
    g_acl_live_users = 0;
    g_acl_initialized = false;
    g_acl_server = nullptr;
}

void acl_update_default_requirepass(Server&, Slice password) {
    std::lock_guard<std::mutex> lock(g_acl_mu);
    if (!g_acl_initialized) return;
    AclUser* user = acl_slot(kAclDefaultUser);
    if (!user) return;
    user->passwords.clear();
    if (password.empty()) user->flags |= AclUserNoPass;
    else {
        user->flags &= ~AclUserNoPass;
        user->passwords.push_back(auth_password_digest(password));
    }
    acl_refresh_security_locked();
}

bool acl_authenticate(Slice username, Slice password, uint32_t& user_index) {
    std::lock_guard<std::mutex> lock(g_acl_mu);
    auto found = g_acl_names.find(std::string(username.p, username.n));
    if (found == g_acl_names.end()) return false;
    AclUser* user = acl_slot(found->second);
    if (!user || !(user->flags & AclUserEnabled)) return false;
    if (!(user->flags & AclUserNoPass)) {
        const auto candidate = auth_password_digest(password);
        bool match = false;
        for (const auto& digest : user->passwords) match |= acl_digest_equal(candidate, digest);
        if (!match) return false;
    }
    user_index = user->index;
    return true;
}

bool acl_find_user(Slice username, uint32_t& user_index) {
    std::lock_guard<std::mutex> lock(g_acl_mu);
    auto found = g_acl_names.find(std::string(username.p, username.n));
    if (found == g_acl_names.end() || !acl_slot(found->second)) return false;
    user_index = found->second;
    return true;
}

bool acl_default_nopass() {
    std::lock_guard<std::mutex> lock(g_acl_mu);
    AclUser* user = acl_slot(kAclDefaultUser);
    return user && (user->flags & AclUserNoPass);
}

const char* acl_username(uint32_t user_index) {
    AclUser* user = acl_slot(user_index);
    return user ? user->name.c_str() : "default";
}

void acl_set_pubsub_default(bool allchannels) {
    std::lock_guard<std::mutex> lock(g_acl_mu);
    g_acl_pubsub_default_all = allchannels;
}

bool acl_channel_allowed_any(const AclPerm& permissions, Slice channel,
                             bool literal_pattern) {
    auto allowed = [&](const AclSelector& selector) {
        return (selector.flags & AclAllChannels) ||
               acl_channel_allowed(selector, channel, literal_pattern);
    };
    if (allowed(permissions)) return true;
    for (const AclSelector& selector : permissions.selectors)
        if (allowed(selector)) return true;
    return false;
}

__attribute__((cold, noinline)) bool acl_finish_dispatch_denial(
        IoLoop& loop, Client& client, Op& op, uint32_t consumed,
        AclDeniedReason denied, uint32_t denied_arg) {
    multi_mark_queue_error(client);
    client.advance_parse(consumed);
    loop.self_->note_command(op.spec->id);
    acl_reply_denied(client, op, client.acl_user_idx(), denied, denied_arg, *loop.self_);
    loop.finish_prebuilt(&client, op);
    return true;
}

bool acl_dispatch_entry(IoLoop& loop, Client& client, Op& op, uint32_t consumed,
                        uint8_t security_flags) {
    const bool unauthenticated = (security_flags & Server::kSecurityAuth) &&
                                 !client.authenticated();
    const bool acl_active = (security_flags & Server::kSecurityAcl) != 0;
    if (unauthenticated && auth_dispatch_entry(loop, client, op, consumed)) {
        return true;
    }
    if (!acl_active) return false;

    const CommandSpec& spec = *op.spec;
    if (spec.flags & CmdFlags::AclExempt) return false;
    uint32_t denied_arg = UINT32_MAX;
    const AclDeniedReason denied = acl_check_permission(
        client.acl_user_idx(), spec, op.argc(),
        [&](uint32_t arg) { return op.arg(arg); }, &denied_arg);
    if (denied == AclDeniedReason::None) return false;
    return acl_finish_dispatch_denial(loop, client, op, consumed, denied, denied_arg);
}

AclDeniedReason acl_check_queued(uint32_t user_index, const CommandSpec& spec,
                                 const std::vector<std::string>& argv,
                                 uint32_t* denied_arg) {
    return acl_check_permission(user_index, spec,
        static_cast<uint32_t>(argv.size()), [&](uint32_t arg) {
            const std::string& value = argv[arg];
            return Slice(value.data(), static_cast<uint32_t>(value.size()));
        }, denied_arg);
}

bool acl_pubsub_channel_entry(Client& client, Op& op, bool pattern, ThreadCtx& thread) {
    AclUser* user = acl_slot(client.acl_user_idx());
    const AclPerm* perm = user ? user->perm.load(std::memory_order_acquire) : nullptr;
    if (!perm) {
        acl_reply_denied(client, op, client.acl_user_idx(), AclDeniedReason::Channel, 1, thread);
        return false;
    }
    const uint32_t first = (op.cmd_name().eq_icase("publish") ||
                            op.cmd_name().eq_icase("spublish")) ? 1 : 1;
    const uint32_t last = (op.cmd_name().eq_icase("publish") ||
                           op.cmd_name().eq_icase("spublish")) ? 2 : op.argc();
    auto allowed = [&](const AclSelector& selector) {
        if (!acl_command_allowed(selector, *op.spec)) return false;
        if (selector.flags & AclAllChannels) return true;
        for (uint32_t arg = first; arg < last; arg++)
            if (!acl_channel_allowed(selector, op.arg(arg), pattern)) return false;
        return true;
    };
    if (allowed(*perm)) return true;
    for (const AclSelector& selector : perm->selectors)
        if (allowed(selector)) return true;
    acl_reply_denied(client, op, client.acl_user_idx(),
                     AclDeniedReason::Channel, first, thread);
    return false;
}

void acl_recheck_blocking(Client& client, Op& op, ThreadCtx& thread) {
    if (!g_acl_server || !g_acl_server->acl_active()) return;
    uint32_t denied_arg = UINT32_MAX;
    const AclDeniedReason denied = acl_check_permission(
        client.acl_user_idx(), *op.spec, op.argc(),
        [&](uint32_t arg) { return op.arg(arg); }, &denied_arg);
    if (denied == AclDeniedReason::None) return;
    op.reply.clear();
    acl_reply_denied(client, op, client.acl_user_idx(), denied, denied_arg, thread);
}

static constexpr SubcommandArity kAclSubcommands[] = {
    {"cat",      2,  3, SubcommandArityError::UnknownOrWrong},
    {"deluser",  3, -1},
    {"genpass",  2,  3, SubcommandArityError::UnknownOrWrong},
    {"getuser",  3,  3},
    {"list",     2,  2},
    {"load",     2,  2},
    {"log",      2,  3, SubcommandArityError::UnknownOrWrong},
    {"save",     2,  2},
    {"setuser",  3, -1},
    {"users",    2,  2},
    {"whoami",   2,  2},
    {"help",     2,  2},
};

void acl_command_entry(IoLoop& loop, Client& client, Op& op) {
    if (!command_validate_subcommand(op, "ACL", kAclSubcommands,
                                     sizeof(kAclSubcommands) / sizeof(kAclSubcommands[0]))) return;
    const Slice sub = op.arg(1);
    if (sub.eq_icase("cat") && (op.argc() == 2 || op.argc() == 3)) {
        acl_reply_category(op);
        return;
    }
    if (sub.eq_icase("setuser") && op.argc() >= 3) {
        if (!acl_valid_username(op.arg(2))) {
            reply_err(op.sink(), "ERR Usernames can't contain spaces or null characters");
            return;
        }
        std::lock_guard<std::mutex> lock(g_acl_mu);
        const std::string name(op.arg(2).p, op.arg(2).n);
        auto found = g_acl_names.find(name);
        AclUser* user = found == g_acl_names.end() ? nullptr : acl_slot(found->second);
        AclUserImage image = user ? acl_image(*user) : acl_new_image(name);
        std::vector<std::string> rules;
        rules.reserve(op.argc() - 3);
        for (uint32_t arg = 3; arg < op.argc(); arg++) {
            const Slice rule = op.arg(arg);
            rules.emplace_back(rule.p, rule.n);
        }
        std::string error, failed;
        if (!acl_apply_rules(image, rules, 0, error, &failed)) {
            std::string message = "ERR ";
            if (error.rfind("Unmatched parenthesis", 0) == 0) message += error;
            else message += "Error in ACL SETUSER modifier '" + failed + "': " + error;
            reply_err(op.sink(), message.c_str());
            return;
        }
        if (user ? !acl_publish_image_locked(user, std::move(image))
                 : !acl_create_locked(std::move(image))) {
            reply_err(op.sink(), "ERR out of memory");
            return;
        }
        if (user)
            acl_broadcast_user_change(
                loop, user->index, user->perm.load(std::memory_order_acquire), false);
        acl_refresh_security_locked();
        reply_ok(op.sink());
        return;
    }
    if (sub.eq_icase("getuser") && op.argc() == 3) {
        std::lock_guard<std::mutex> lock(g_acl_mu);
        auto found = g_acl_names.find(std::string(op.arg(2).p, op.arg(2).n));
        if (found == g_acl_names.end()) { reply_null(op.sink(), op.resp3()); return; }
        AclUser* user = acl_slot(found->second);
        const AclUserImage image = acl_image(*user);
        auto sink = op.sink();
        reply_array_header(sink, 12);
        reply_bulk(sink, Slice("flags", 5));
        uint32_t nflags = !!(image.flags & AclUserEnabled) + !!(image.flags & AclUserDisabled) +
                          !!(image.flags & AclUserNoPass) + !!(image.flags & AclUserSanitize) +
                          !!(image.flags & AclUserSkipSanitize);
        reply_array_header(sink, nflags);
        if (image.flags & AclUserEnabled) reply_bulk(sink, Slice("on", 2));
        if (image.flags & AclUserDisabled) reply_bulk(sink, Slice("off", 3));
        if (image.flags & AclUserNoPass) reply_bulk(sink, Slice("nopass", 6));
        if (image.flags & AclUserSkipSanitize) reply_bulk(sink, Slice("skip-sanitize-payload", 21));
        if (image.flags & AclUserSanitize) reply_bulk(sink, Slice("sanitize-payload", 16));
        reply_bulk(sink, Slice("passwords", 9));
        reply_array_header(sink, image.passwords.size());
        for (const auto& password : image.passwords) {
            const std::string text = acl_hex_hash(password);
            reply_bulk(sink, Slice(text.data(), text.size()));
        }
        reply_bulk(sink, Slice("commands", 8));
        const std::string commands = acl_describe_commands(image.perm);
        reply_bulk(sink, Slice(commands.data(), commands.size()));
        reply_bulk(sink, Slice("keys", 4));
        const std::string keys = acl_join_patterns(image.perm, false);
        reply_bulk(sink, Slice(keys.data(), keys.size()));
        reply_bulk(sink, Slice("channels", 8));
        const std::string channels = acl_getuser_channels(image.perm);
        reply_bulk(sink, Slice(channels.data(), channels.size()));
        reply_bulk(sink, Slice("selectors", 9));
        reply_array_header(sink, image.perm.selectors.size());
        for (const AclSelector& selector : image.perm.selectors) {
            reply_array_header(sink, 6);
            reply_bulk(sink, Slice("commands", 8));
            const std::string selector_commands = acl_describe_commands(selector);
            reply_bulk(sink, Slice(selector_commands.data(), selector_commands.size()));
            reply_bulk(sink, Slice("keys", 4));
            const std::string selector_keys = acl_join_patterns(selector, false);
            reply_bulk(sink, Slice(selector_keys.data(), selector_keys.size()));
            reply_bulk(sink, Slice("channels", 8));
            const std::string selector_channels = acl_getuser_channels(selector);
            reply_bulk(sink, Slice(selector_channels.data(), selector_channels.size()));
        }
        return;
    }
    if ((sub.eq_icase("users") || sub.eq_icase("list")) && op.argc() == 2) {
        std::lock_guard<std::mutex> lock(g_acl_mu);
        const auto users = acl_sorted_users_locked();
        reply_array_header(op.sink(), users.size());
        for (AclUser* user : users) {
            std::string text;
            if (sub.eq_icase("list")) text = "user " + user->name + " " +
                                             acl_describe_image(acl_image(*user));
            else text = user->name;
            reply_bulk(op.sink(), Slice(text.data(), text.size()));
        }
        return;
    }
    if (sub.eq_icase("whoami") && op.argc() == 2) {
        const char* name = acl_username(client.acl_user_idx());
        reply_bulk(op.sink(), Slice(name, std::strlen(name)));
        return;
    }
    if (sub.eq_icase("deluser") && op.argc() >= 3) {
        for (uint32_t arg = 2; arg < op.argc(); arg++)
            if (op.arg(arg) == Slice("default", 7)) {
                reply_err(op.sink(), "ERR The 'default' user cannot be removed");
                return;
            }
        std::lock_guard<std::mutex> lock(g_acl_mu);
        long long deleted = 0;
        for (uint32_t arg = 2; arg < op.argc(); arg++) {
            auto found = g_acl_names.find(std::string(op.arg(arg).p, op.arg(arg).n));
            if (found == g_acl_names.end()) continue;
            acl_broadcast_user_change(loop, found->second, nullptr, true);
            acl_tombstone_user_locked(found->second);
            g_acl_names.erase(found);
            g_acl_live_users--;
            deleted++;
        }
        acl_refresh_security_locked();
        reply_int(op.sink(), deleted);
        return;
    }
    if (sub.eq_icase("log") && (op.argc() == 2 || op.argc() == 3)) {
        acl_reply_log(op);
        return;
    }
    if ((sub.eq_icase("load") || sub.eq_icase("save")) && g_acl_filename.empty()) {
        reply_err(op.sink(), kAclNoFileError);
        return;
    }
    if (sub.eq_icase("load") && op.argc() == 2) {
        std::vector<AclUserImage> images;
        std::string error;
        if (!acl_parse_file(g_acl_filename, images, error)) {
            reply_err(op.sink(), error.c_str());
            return;
        }
        std::lock_guard<std::mutex> lock(g_acl_mu);
        if (!acl_commit_images_locked(images, &loop)) reply_err(op.sink(), "ERR out of memory");
        else reply_ok(op.sink());
        return;
    }
    if (sub.eq_icase("save") && op.argc() == 2) {
        std::lock_guard<std::mutex> lock(g_acl_mu);
        if (acl_save_file_locked()) reply_ok(op.sink());
        else reply_err(op.sink(), "ERR There was an error trying to save the ACLs. Please check the server logs for more information");
        return;
    }
    if (sub.eq_icase("genpass") && (op.argc() == 2 || op.argc() == 3)) {
        uint64_t bits = 256;
        if (op.argc() == 3) {
            bits = 0;
            for (uint32_t i = 0; i < op.arg(2).n; i++) {
                const char byte = op.arg(2).p[i];
                if (byte < '0' || byte > '9' || bits > 4096) { bits = 0; break; }
                bits = bits * 10 + static_cast<uint64_t>(byte - '0');
            }
        }
        if (!bits || bits > 4096) {
            reply_err(op.sink(), "ERR ACL GENPASS argument must be the number of bits for the output password, a positive number up to 4096");
            return;
        }
        const size_t chars = (bits + 3) / 4;
        std::string random(chars, '0');
        if (::getrandom(random.data(), random.size(), 0) != static_cast<ssize_t>(random.size())) {
            reply_err(op.sink(), "ERR out of memory");
            return;
        }
        static constexpr char hex[] = "0123456789abcdef";
        for (char& byte : random) byte = hex[static_cast<unsigned char>(byte) & 15];
        reply_bulk(op.sink(), Slice(random.data(), random.size()));
        return;
    }
    if (sub.eq_icase("help") && op.argc() == 2) {
        static constexpr const char* help[] = {
            "ACL <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
            "CAT [<category>]", "DELUSER <username> [<username> ...]", "GENPASS [<bits>]",
            "GETUSER <username>", "LIST", "LOAD", "SAVE",
            "LOG [<count> | RESET]", "SETUSER <username> [<rule> [<rule> ...]]",
            "USERS", "WHOAMI", "HELP"
        };
        reply_array_header(op.sink(), sizeof(help) / sizeof(help[0]));
        for (const char* line : help) reply_simple(op.sink(), line);
        return;
    }
    reply_err(op.sink(), "ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.");
}

void cmd_acl(Shard&, Op& op) {
    // ACL is executed only by acl_command_entry, which has the owning IO/client context.
    reply_err(op.sink(), "ERR ACL is not supported in this execution context");
}

}  // namespace tomo
