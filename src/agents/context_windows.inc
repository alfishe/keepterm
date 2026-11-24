// Built-in context windows of model ids.
// KT_WINDOW(prefix, tokens): the first row whose prefix starts the model id wins, so longer
// prefixes come first. agents.context.windows overrides this table. Codex and Qoder report the
// window in their own records and need no row.

// Claude: newer models have 1M natively; older ones 200k (1M only when selected with "[1m]",
// which the transcript does not show; see the evidence rule in context_claude.cpp).
KT_WINDOW("claude-opus-4-7", 1'000'000)
KT_WINDOW("claude-opus-4-8", 1'000'000)
KT_WINDOW("claude-opus-4-9", 1'000'000)
KT_WINDOW("claude-opus-5", 1'000'000)
KT_WINDOW("claude-sonnet-5", 1'000'000)
KT_WINDOW("claude-haiku-5-5", 1'000'000)
KT_WINDOW("claude-fable", 1'000'000)
KT_WINDOW("claude-", 200'000)

// Gemini (the Antigravity CLI): 1M (1,048,576) for the current models.
KT_WINDOW("gemini-", 1'048'576)
