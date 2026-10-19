#include "anomaly/plugin_capability_policy.hpp"

#include <iostream>

namespace {

bool Check(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    anomaly::PluginManifest manifest;
    manifest.schema_version = anomaly::kLatestPluginManifestSchemaVersion;
    manifest.id = "anomaly.builtin.nte-damage-chain-reaction";
    manifest.capabilities = {
        "ui",
        "nte-combat-read",
        "game-events",
        "nte-damage-chain-reaction",
        "storage",
    };
    manifest.services = {
        {"anomaly.ui", 1, false},
        {"anomaly.nte.combat", 1, false},
        {"anomaly.nte.damage-chain-reaction", 1, false},
        {"anomaly.storage", 1, true},
    };

    const auto grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    if (!Check(grant.IsEnforceable(),
            "the current Damage Chain Reaction manifest must have an enforceable grant")) {
        for (const auto& audit : grant.Audits()) {
            std::cerr << "  audit code=" << static_cast<unsigned>(audit.code)
                      << " capability=" << audit.capability
                      << " service=" << audit.service << '\n';
        }
        return 1;
    }

    const auto authorization =
        grant.AuthorizeService("anomaly.nte.damage-chain-reaction");
    if (!Check(authorization.allowed,
            "the registered Damage Chain Reaction service must be authorized")) return 2;
    if (!Check(authorization.required_capability == "nte-damage-chain-reaction",
            "the service must map to its dedicated capability")) return 3;

    // The new service must not inherit authorization from the legacy chain capability.
    anomaly::PluginManifest legacy_only;
    legacy_only.schema_version = anomaly::kLatestPluginManifestSchemaVersion;
    legacy_only.id = "test.legacy-chain-only";
    legacy_only.capabilities = {"nte-damage-chain"};
    legacy_only.services = {
        {"anomaly.nte.damage-chain-reaction", 1, false},
    };
    const auto legacy_grant = anomaly::ResolvePluginCapabilityGrant(&legacy_only);
    if (!Check(!legacy_grant.IsEnforceable(),
            "the legacy capability alone must not authorize the replacement service")) return 4;
    if (!Check(!legacy_grant.AuthorizeService(
                    "anomaly.nte.damage-chain-reaction").allowed,
            "the replacement service authorization must remain fail-closed")) return 5;

    std::cout << "Damage Chain Reaction capability policy checks passed.\n";
    return 0;
}
