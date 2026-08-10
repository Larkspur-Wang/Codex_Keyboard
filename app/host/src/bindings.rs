use thiserror::Error;

use crate::codex_catalog::{CatalogError, CodexTask, CodexTaskCatalog};
use crate::store::{Binding, StateStore, StoreError};

#[derive(Debug, Error)]
pub enum BindingError {
    #[error("Codex task catalog failed: {0}")]
    Catalog(#[from] CatalogError),
    #[error("Host state failed: {0}")]
    Store(#[from] StoreError),
}

pub struct BindingService<'a> {
    catalog: &'a CodexTaskCatalog,
}

impl<'a> BindingService<'a> {
    pub fn new(catalog: &'a CodexTaskCatalog) -> Self {
        Self { catalog }
    }

    pub fn bind(
        &self,
        store: &mut StateStore,
        slot: u8,
        expected_generation: Option<u64>,
        task_id: &str,
    ) -> Result<Option<Binding>, BindingError> {
        self.catalog.allowlisted(task_id)?;
        Ok(store.set_binding(slot, expected_generation, task_id)?)
    }

    pub fn resolve(
        &self,
        store: &StateStore,
        slot: u8,
    ) -> Result<Option<(Binding, CodexTask)>, BindingError> {
        let Some(binding) = store.binding(slot)? else {
            return Ok(None);
        };
        let task = self.catalog.allowlisted(&binding.task_id)?;
        Ok(Some((binding, task)))
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    use rusqlite::{Connection, params};
    use serde_json::json;
    use tempfile::tempdir;

    use super::*;

    const TASK_A: &str = "019fa972-5cfa-75e1-9008-0b17ade9a347";

    fn write_catalog(home: &Path) {
        fs::create_dir_all(home.join("sessions")).unwrap();
        fs::write(home.join("sessions/a.jsonl"), b"").unwrap();
        fs::write(
            home.join(".codex-global-state.json"),
            serde_json::to_vec(&json!({
                "pinned-thread-ids": [TASK_A],
                "electron-persisted-atom-state": {}
            }))
            .unwrap(),
        )
        .unwrap();
        fs::write(
            home.join("session_index.jsonl"),
            format!("{{\"id\":\"{TASK_A}\",\"thread_name\":\"Task A\"}}\n"),
        )
        .unwrap();
        let connection = Connection::open(home.join("state_5.sqlite")).unwrap();
        connection
            .execute_batch(
                "CREATE TABLE threads (
                    id TEXT PRIMARY KEY, name TEXT, title TEXT NOT NULL, cwd TEXT NOT NULL,
                    rollout_path TEXT NOT NULL, updated_at INTEGER NOT NULL,
                    updated_at_ms INTEGER, recency_at_ms INTEGER NOT NULL,
                    archived INTEGER NOT NULL, source TEXT, thread_source TEXT,
                    agent_role TEXT
                 );",
            )
            .unwrap();
        connection
            .execute(
                "INSERT INTO threads VALUES
                 (?1, 'Task A', '', '/work/a', ?2, 1, 1000, 1000, 0,
                  'vscode', 'user', NULL)",
                params![TASK_A, home.join("sessions/a.jsonl").to_string_lossy()],
            )
            .unwrap();
    }

    #[test]
    fn binding_is_catalog_gated_cas_and_revalidated_on_resolve() {
        let temp = tempdir().unwrap();
        let codex_home = temp.path().join("codex");
        let host_home = temp.path().join("host");
        write_catalog(&codex_home);
        let snapshot_root = temp.path().join("catalog-snapshots");
        crate::paths::secure_directory(&snapshot_root).unwrap();
        let catalog = CodexTaskCatalog::from_paths(codex_home.clone(), snapshot_root);
        let service = BindingService::new(&catalog);
        let mut store = StateStore::open(&host_home.join("state.sqlite3")).unwrap();

        let first = service.bind(&mut store, 1, None, TASK_A).unwrap().unwrap();
        assert_eq!(first.generation, 1);
        assert!(service.bind(&mut store, 1, None, TASK_A).unwrap().is_none());
        assert_eq!(service.resolve(&store, 1).unwrap().unwrap().0, first);
        assert!(matches!(
            service.bind(&mut store, 2, None, "019f0000-0000-7000-8000-000000000000"),
            Err(BindingError::Catalog(CatalogError::NotAllowlisted))
        ));

        let connection = Connection::open(codex_home.join("state_5.sqlite")).unwrap();
        connection
            .execute("UPDATE threads SET archived = 1 WHERE id = ?1", [TASK_A])
            .unwrap();
        drop(connection);
        assert!(matches!(
            service.resolve(&store, 1),
            Err(BindingError::Catalog(CatalogError::NotAllowlisted))
        ));
    }
}
