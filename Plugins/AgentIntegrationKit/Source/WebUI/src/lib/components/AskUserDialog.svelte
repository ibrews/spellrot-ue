<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		MessageQuestionIcon,
		Tick02Icon,
		Cancel01Icon
	} from '@hugeicons/core-free-icons';
	import type { PermissionRequest, Question } from '$lib/bridge.js';
	import { respondToQuestions, skipQuestions } from '$lib/stores/permissions.js';

	let { request }: { request: PermissionRequest } = $props();

	// Per-question answer state
	let singleAnswers: Record<string, string> = $state({});
	let multiAnswers: Record<string, Set<string>> = $state({});
	let otherText: Record<string, string> = $state({});
	let responding = $state(false);

	// Keyboard navigation state
	let activeQuestionIndex = $state(0);
	let activeOptionByQuestion: Record<number, number> = $state({});
	let rootEl: HTMLDivElement | undefined = $state();
	let currentQuestion = $derived(request.questions[activeQuestionIndex]);
	let currentOtherOptionIndex = $derived(currentQuestion ? currentQuestion.options.length : 0);
	let currentOtherActive = $derived(
		currentQuestion ? getActiveOptionIndex(activeQuestionIndex) === currentQuestion.options.length : false
	);

	$effect(() => {
		request.requestId;
		singleAnswers = {};
		multiAnswers = {};
		otherText = {};
		responding = false;
		activeQuestionIndex = 0;
		activeOptionByQuestion = {};
		requestAnimationFrame(() => {
			const firstTab = rootEl?.querySelector<HTMLButtonElement>('button[data-question-tab="0"]');
			firstTab?.focus();
		});
	});

	function selectSingle(question: string, label: string) {
		singleAnswers = { ...singleAnswers, [question]: label };
		const nextOther = { ...otherText };
		delete nextOther[question];
		otherText = nextOther;
	}

	function toggleMulti(question: string, label: string) {
		const set = multiAnswers[question] ? new Set(multiAnswers[question]) : new Set<string>();
		if (set.has(label)) {
			set.delete(label);
		} else {
			set.add(label);
		}
		multiAnswers = { ...multiAnswers, [question]: set };
	}

	function isMultiSelected(question: string, label: string): boolean {
		return multiAnswers[question]?.has(label) ?? false;
	}

	function handleOtherInput(question: string, value: string) {
		const nextOther = { ...otherText };
		if (value) {
			nextOther[question] = value;
		} else {
			delete nextOther[question];
		}
		otherText = nextOther;
	}

	function optionCount(q: Question): number {
		return q.options.length + 1; // +1 for "Other"
	}

	function getActiveOptionIndex(questionIndex: number): number {
		const question = request.questions[questionIndex];
		if (!question) return 0;
		const max = optionCount(question) - 1;
		const current = activeOptionByQuestion[questionIndex] ?? 0;
		return Math.max(0, Math.min(current, max));
	}

	function setActiveOptionIndex(questionIndex: number, next: number) {
		const question = request.questions[questionIndex];
		if (!question) return;
		const count = optionCount(question);
		activeOptionByQuestion = {
			...activeOptionByQuestion,
			[questionIndex]: (next + count) % count
		};
	}

	function setActiveQuestionIndex(next: number) {
		const count = request.questions.length;
		if (count <= 0) return;
		activeQuestionIndex = (next + count) % count;
	}

	function isQuestionAnswered(q: Question): boolean {
		if (otherText[q.question]) return true;
		if (q.multiSelect) {
			return (multiAnswers[q.question]?.size ?? 0) > 0;
		}
		return !!singleAnswers[q.question];
	}

	function selectCurrentOption() {
		const q = request.questions[activeQuestionIndex];
		if (!q) return;

		const optionIndex = getActiveOptionIndex(activeQuestionIndex);
		if (optionIndex === q.options.length) {
			const input = rootEl?.querySelector<HTMLInputElement>(
				`input[data-question-index=\"${activeQuestionIndex}\"]`
			);
			input?.focus();
			return;
		}

		const option = q.options[optionIndex];
		if (!option) return;
		if (q.multiSelect) {
			toggleMulti(q.question, option.label);
		} else {
			selectSingle(q.question, option.label);
		}
	}

	async function handleSubmit() {
		if (responding) return;
		responding = true;

		// Build answers map: question text -> answer text
		const answers: Record<string, string> = {};
		for (const q of request.questions) {
			if (otherText[q.question]) {
				answers[q.question] = otherText[q.question];
			} else if (q.multiSelect) {
				const selected = multiAnswers[q.question];
				if (selected && selected.size > 0) {
					answers[q.question] = [...selected].join(', ');
				}
			} else {
				const selected = singleAnswers[q.question];
				if (selected) {
					answers[q.question] = selected;
				}
			}
		}

		await respondToQuestions(answers);
	}

	async function handleSkip() {
		if (responding) return;
		responding = true;
		await skipQuestions();
	}

	function handleKeydown(e: KeyboardEvent) {
		if (!e.metaKey && !e.ctrlKey && !e.altKey) {
			e.stopPropagation();
		}
		if (responding || request.questions.length === 0) return;

		if (e.key === 'Tab') {
			e.preventDefault();
			setActiveQuestionIndex(activeQuestionIndex + (e.shiftKey ? -1 : 1));
			return;
		}

		if (e.key === 'ArrowRight') {
			e.preventDefault();
			setActiveQuestionIndex(activeQuestionIndex + 1);
			return;
		}

		if (e.key === 'ArrowLeft') {
			e.preventDefault();
			setActiveQuestionIndex(activeQuestionIndex - 1);
			return;
		}

		if (e.key === 'ArrowDown') {
			e.preventDefault();
			setActiveOptionIndex(activeQuestionIndex, getActiveOptionIndex(activeQuestionIndex) + 1);
			return;
		}

		if (e.key === 'ArrowUp') {
			e.preventDefault();
			setActiveOptionIndex(activeQuestionIndex, getActiveOptionIndex(activeQuestionIndex) - 1);
			return;
		}

		if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') {
			e.preventDefault();
			handleSubmit();
			return;
		}

		if (e.key === 'Enter') {
			e.preventDefault();
			selectCurrentOption();
		}
	}
</script>

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div
	bind:this={rootEl}
	role="application"
	aria-label="Questionnaire"
	onkeydown={handleKeydown}
	class="rounded-xl border border-blue-500/30 bg-blue-500/5 p-3 focus:outline-none focus:ring-2 focus:ring-[var(--ue-accent-muted)]"
>
	<div class="mb-2 flex items-center justify-between gap-2">
		<div class="flex items-center gap-2">
			<Icon icon={MessageQuestionIcon} size={18} strokeWidth={1.5} class="text-blue-400" />
			<span class="text-[14px] font-semibold text-blue-400">Questionnaire</span>
		</div>
		<span class="text-[11px] text-muted-foreground/75">Tab switch • ↑/↓ select • Enter apply • Ctrl+Enter submit</span>
	</div>

	<!-- Question tabs -->
	<div class="mb-3 flex flex-wrap gap-1.5">
		{#each request.questions as q, qi}
			{@const answered = isQuestionAnswered(q)}
			<button
				data-question-tab={qi}
				class="flex items-center gap-1.5 rounded-md border px-2.5 py-1 text-[12px] transition-all {qi === activeQuestionIndex
					? 'border-blue-500/60 bg-blue-500/15 text-blue-200'
					: 'border-border/35 bg-secondary/20 text-muted-foreground hover:border-border/60'}"
				onclick={() => (activeQuestionIndex = qi)}
				onmouseenter={() => (activeQuestionIndex = qi)}
			>
				<span>{q.header || `Question ${qi + 1}`}</span>
				{#if answered}
					<Icon icon={Tick02Icon} size={12} strokeWidth={2.5} class="text-emerald-400" />
				{/if}
			</button>
		{/each}
	</div>

	{#if currentQuestion}
		<div class="rounded-lg border border-border/30 bg-card/40 p-3">
			<div class="mb-2 text-[13px] font-medium text-foreground">{currentQuestion.question}</div>

			<div class="flex flex-col gap-1.5">
				{#each currentQuestion.options as opt, oi}
					{@const isSelected = currentQuestion.multiSelect
						? isMultiSelected(currentQuestion.question, opt.label)
						: singleAnswers[currentQuestion.question] === opt.label}
					{@const isActive = getActiveOptionIndex(activeQuestionIndex) === oi}
					<button
						class="flex items-start gap-2 rounded-lg border px-3 py-2 text-left text-[13px] transition-all
							{isSelected
								? 'border-blue-500/55 bg-blue-500/10'
								: 'border-border/30 bg-transparent hover:border-border/60 hover:bg-secondary/20'}
							{isActive ? 'ring-2 ring-[var(--ue-accent-muted)]' : ''}"
						onclick={() => {
							setActiveOptionIndex(activeQuestionIndex, oi);
							if (currentQuestion.multiSelect) {
								toggleMulti(currentQuestion.question, opt.label);
							} else {
								selectSingle(currentQuestion.question, opt.label);
							}
						}}
						onmouseenter={() => setActiveOptionIndex(activeQuestionIndex, oi)}
					>
						<span class="mt-0.5 flex h-5 w-5 shrink-0 items-center justify-center rounded text-[11px] font-bold
							{isSelected ? 'bg-blue-500 text-white' : 'bg-secondary/60 text-muted-foreground'}"
						>
							{#if isSelected}
								<Icon icon={Tick02Icon} size={12} strokeWidth={2.5} />
							{:else}
								{oi + 1}
							{/if}
						</span>
						<div class="min-w-0 flex-1">
							<div class="text-[13px] text-foreground">{opt.label}</div>
							{#if opt.description}
								<div class="mt-0.5 text-[12px] text-muted-foreground/60">{opt.description}</div>
							{/if}
						</div>
					</button>
				{/each}

				<!-- Other -->
				<div
					class="flex items-start gap-2 rounded-lg border px-3 py-2 transition-all
						{otherText[currentQuestion.question]
							? 'border-blue-500/55 bg-blue-500/10'
							: 'border-border/30 bg-transparent'}
						{currentOtherActive ? 'ring-2 ring-[var(--ue-accent-muted)]' : ''}"
				>
					<span class="mt-0.5 flex h-5 w-5 shrink-0 items-center justify-center rounded text-[11px] font-bold
						{otherText[currentQuestion.question]
							? 'bg-blue-500 text-white'
							: 'bg-secondary/60 text-muted-foreground'}"
					>
						{#if otherText[currentQuestion.question]}
							<Icon icon={Tick02Icon} size={12} strokeWidth={2.5} />
						{:else}
							{currentOtherOptionIndex + 1}
						{/if}
					</span>
					<input
						type="text"
						data-question-index={activeQuestionIndex}
						placeholder="Other..."
						class="min-w-0 flex-1 bg-transparent text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:outline-none"
						value={otherText[currentQuestion.question] ?? ''}
						onfocus={() => setActiveOptionIndex(activeQuestionIndex, currentOtherOptionIndex)}
						oninput={(e) => handleOtherInput(currentQuestion.question, e.currentTarget.value)}
					/>
				</div>
			</div>
		</div>
	{/if}

	<div class="mt-3 flex items-center gap-2">
		<button
			class="flex items-center gap-1.5 rounded-lg bg-emerald-600 px-4 py-1.5 text-[13px] font-medium text-white transition-all hover:bg-emerald-500 {responding
				? 'cursor-not-allowed opacity-50'
				: ''}"
			onclick={handleSubmit}
			disabled={responding}
		>
			<Icon icon={Tick02Icon} size={14} strokeWidth={2} />
			Submit
		</button>
		<button
			class="flex items-center gap-1.5 rounded-lg bg-secondary/60 px-4 py-1.5 text-[13px] font-medium text-muted-foreground transition-all hover:bg-secondary hover:text-foreground {responding
				? 'cursor-not-allowed opacity-50'
				: ''}"
			onclick={handleSkip}
			disabled={responding}
		>
			<Icon icon={Cancel01Icon} size={14} strokeWidth={2} />
			Skip
		</button>
	</div>
</div>
