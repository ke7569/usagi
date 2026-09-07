## ADDED Requirements

### Requirement: Common implementations and symmetric market ownership
The repository SHALL expose a common implementation layer and symmetric Shenzhen/Shanghai difference directories. Common model engines MUST be classified by architecture rather than duplicated by market. Market-selecting application assembly MUST remain outside lower-level common libraries.

#### Scenario: Build one market independently
- **WHEN** one portable market runtime is selected and the other is disabled
- **THEN** it builds without the other market's runtime or unrelated broker SDK and preserves its exported runtime API

### Requirement: Behavior and state preservation during relocation
The migration MUST preserve existing sampling rules, model arithmetic, artifact contracts, recording layouts, instance-state isolation and execution gates. It MUST retain all pre-existing worktree changes and report unresolved live acceptance separately.

#### Scenario: Replay a fixed pre-migration fixture
- **WHEN** the migrated runtime processes the same validated input, model and configuration
- **THEN** samples, factors, predictions and order intents match the pre-migration behavior and replay cannot enable real trading

### Requirement: Relocation-safe repository tooling
Repository build, test, configuration and packaging tools SHALL resolve maintained source assets from the new layout. Production installation paths, credentials and legacy plugin output names MUST NOT change implicitly with the repository name.

#### Scenario: Invoke tooling after the source root is renamed
- **WHEN** a supported tool is invoked from outside `/home/usagi`
- **THEN** repository-local defaults resolve to the migrated assets and external deployment settings retain their explicit configured meaning

### Requirement: Discoverable current documentation
The README SHALL identify Usagi as one repository for both markets and link the current architecture, configuration and operations documentation. Historical execution evidence MUST retain provenance and unfinished acceptance MUST NOT be presented as complete.

#### Scenario: Locate current runtime support
- **WHEN** a maintainer follows the README documentation links
- **THEN** they can distinguish portable paper processing from incomplete legacy-host and real-broker integration without consulting dated execution logs
