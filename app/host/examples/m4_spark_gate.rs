use easy_codex_host::spark_runner::{
    SPARK_MAX_WORKSPACE_BYTES, SPARK_MAX_WORKSPACE_NODES, SPARK_MODEL, SparkRunner,
    SparkRunnerConfig, verify_spark_prompt_isolation,
};
use easy_codex_host::store::{
    PendingSummaryCompletion, SummaryClaim, SummaryClaimOutcome, UnreadSummary,
};
use easy_codex_host::summary::SummaryDocument;

const TASK: &str = "00000000-0000-4000-8000-000000000001";
const PREVIOUS_COMPLETION: &str = "00000000-0000-4000-8000-000000000002";
const NEW_COMPLETION: &str = "00000000-0000-4000-8000-000000000003";

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let previous = SummaryDocument {
        schema: 1,
        facts: vec!["The bounded summary ledger is complete.".into()],
        pending: vec!["Implement the isolated Spark runner.".into()],
        decisions: vec!["Keep summary plaintext out of SQLite.".into()],
        spoken_text: "The summary ledger is complete, and the isolated runner is next.".into(),
        source_evidence: vec![],
        covers_new_completions: vec![PREVIOUS_COMPLETION.into()],
    };
    let claim = SummaryClaim {
        outcome: SummaryClaimOutcome::Inserted,
        request_id: "public-fixture-request".into(),
        task_id: TASK.into(),
        generation: 2,
        previous_unread: Some(UnreadSummary {
            task_id: TASK.into(),
            generation: 1,
            cache_object: "public-fixture-cache".into(),
            coverage_count: 1,
        }),
        completions: vec![PendingSummaryCompletion {
            completion_id: NEW_COMPLETION.into(),
            turn_pack: serde_json::json!({
                "v": 1,
                "turn_id": NEW_COMPLETION,
                "user": ["Finish the isolated Spark runner and its failure tests."],
                "assistant": ["The runner and its bounded tests are implemented."],
                "tools": [{"name": "cargo test", "status": "passed"}]
            })
            .to_string(),
        }],
    };
    let config = SparkRunnerConfig {
        supervisor_executable: None,
        ..SparkRunnerConfig::default()
    };
    let prompt_gate_root = config
        .temp_root
        .parent()
        .ok_or("Spark runtime root has no parent")?
        .join("spark-prompt-gates");
    let prompt_isolation = verify_spark_prompt_isolation(&config.executable, &prompt_gate_root)
        .map_err(|error| format!("prompt isolation gate: {error}"))?;
    let outcome = SparkRunner::new(config)
        .run_with_report(&claim, Some(&previous))
        .map_err(|error| format!("Spark summary gate: {error}"))?;
    let document = outcome.document;
    if document.covers_new_completions != [NEW_COMPLETION] {
        return Err("Spark returned the wrong completion delta".into());
    }
    if outcome.isolation.system_skill_files != 0
        || outcome.isolation.plugin_files != 0
        || outcome.isolation.persistent_runtime_rows != 0
        || outcome.isolation.workspace_max_nodes > SPARK_MAX_WORKSPACE_NODES
        || outcome.isolation.workspace_max_bytes > SPARK_MAX_WORKSPACE_BYTES
        || prompt_isolation.skills_instruction_blocks != 0
        || prompt_isolation.skill_path_mentions != 0
    {
        return Err("Spark isolation gate failed".into());
    }
    println!(
        "{}",
        serde_json::json!({
            "status": "pass",
            "model": SPARK_MODEL,
            "schema": document.schema,
            "facts_count": document.facts.len(),
            "pending_count": document.pending.len(),
            "decisions_count": document.decisions.len(),
            "spoken_text_bytes": document.spoken_text.len(),
            "coverage_count": document.covers_new_completions.len(),
            "workspace_max_nodes": outcome.isolation.workspace_max_nodes,
            "workspace_max_bytes": outcome.isolation.workspace_max_bytes,
            "system_skill_files": outcome.isolation.system_skill_files,
            "plugin_files": outcome.isolation.plugin_files,
            "persistent_runtime_rows": outcome.isolation.persistent_runtime_rows,
            "prompt_items": prompt_isolation.prompt_items,
            "prompt_skills_instruction_blocks": prompt_isolation.skills_instruction_blocks,
            "prompt_skill_path_mentions": prompt_isolation.skill_path_mentions,
            "prompt_workspace_max_nodes": prompt_isolation.workspace_max_nodes,
            "prompt_workspace_max_bytes": prompt_isolation.workspace_max_bytes,
            "summary_text_emitted": false,
            "private_identifiers_emitted": false
        })
    );
    Ok(())
}
