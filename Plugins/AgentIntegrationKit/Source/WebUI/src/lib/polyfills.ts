function normalizeAtIndex(index: number, length: number): number {
	const normalized = Number.isFinite(index) ? Math.trunc(index) : 0;
	return normalized >= 0 ? normalized : length + normalized;
}

export function applyRuntimePolyfills(): void {
	// Older embedded browser runtimes may not support at().
	if (!Array.prototype.at) {
		Object.defineProperty(Array.prototype, 'at', {
			value: function at<T>(this: T[], index: number): T | undefined {
				const i = normalizeAtIndex(index, this.length);
				return i >= 0 && i < this.length ? this[i] : undefined;
			},
			writable: true,
			configurable: true
		});
	}

	if (!String.prototype.at) {
		Object.defineProperty(String.prototype, 'at', {
			value: function at(this: string, index: number): string | undefined {
				const i = normalizeAtIndex(index, this.length);
				return i >= 0 && i < this.length ? this.charAt(i) : undefined;
			},
			writable: true,
			configurable: true
		});
	}
}

applyRuntimePolyfills();
