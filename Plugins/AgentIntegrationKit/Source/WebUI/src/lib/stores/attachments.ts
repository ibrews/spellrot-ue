import { writable, derived } from 'svelte/store';
import {
	pasteClipboardImage,
	openImagePicker,
	addImageFromBase64,
	addFileFromBase64,
	removeAttachment,
	onAttachmentsChanged,
	type AttachmentInfo
} from '$lib/bridge.js';

export const attachments = writable<AttachmentInfo[]>([]);
export const hasAttachments = derived(attachments, ($a) => $a.length > 0);

// ── Binding ──────────────────────────────────────────────────────────

let bound = false;

/** Wire up attachment change callbacks. Call once on mount. */
export function bindAttachmentsListener(): void {
	if (bound) return;
	bound = true;

	onAttachmentsChanged((list) => {
		attachments.set(list);
	});
}

// ── Actions ──────────────────────────────────────────────────────────

/** Paste image from system clipboard */
export async function pasteImage(): Promise<boolean> {
	const result = await pasteClipboardImage();
	return result.success;
}

/** Open native file picker for attachments (images + common docs) */
export async function pickAttachments(): Promise<void> {
	await openImagePicker();
}

function arrayBufferToBase64(buffer: ArrayBuffer): string {
	const bytes = new Uint8Array(buffer);
	let binary = '';
	const chunkSize = 0x8000;

	for (let i = 0; i < bytes.length; i += chunkSize) {
		const chunk = bytes.subarray(i, i + chunkSize);
		binary += String.fromCharCode(...chunk);
	}

	return btoa(binary);
}

/** Add a dropped file (image or document) */
export async function addDroppedFile(file: File): Promise<void> {
	if (file.type.startsWith('image/')) {
		const reader = new FileReader();
		reader.onload = async () => {
			const dataUrl = reader.result as string;
			const commaIdx = dataUrl.indexOf(',');
			if (commaIdx < 0) return;
			const base64 = dataUrl.slice(commaIdx + 1);

			const img = new Image();
			img.onload = async () => {
				await addImageFromBase64(base64, file.type || 'image/png', img.width, img.height, file.name);
			};
			img.src = dataUrl;
		};
		reader.readAsDataURL(file);
		return;
	}

	const buffer = await file.arrayBuffer();
	const base64 = arrayBufferToBase64(buffer);
	await addFileFromBase64(base64, file.type || 'application/octet-stream', file.name);
}

/** Remove an attachment by ID */
export async function removeItem(id: string): Promise<void> {
	await removeAttachment(id);
}
