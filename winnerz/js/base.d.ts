import type { PdfiumModule } from './vendor/pdfium';
import type { PdfiumRuntimeMethods } from './vendor/runtime-methods';
import { functions } from './vendor/functions';
export type { PdfiumModule } from './vendor/pdfium';
export type { PdfiumRuntimeMethods } from './vendor/runtime-methods';
export declare const DEFAULT_PDFIUM_WASM_URL: string;
/**
 * Name of JavaScript type
 */
export type Type = null | 'number' | 'string' | 'boolean' | null;
/**
 * Type of wrapped function
 */
export type CWrappedFunc<I extends readonly Type[], R extends Type> = (...args: NamesToType<I>) => NameToType<R>;
/**
 * Convert name to type
 */
export type NameToType<R extends Type> = R extends 'number' ? number : R extends 'string' ? string : R extends 'boolean' ? boolean : R extends null ? null : never;
/**
 * Convert array of names to JavaScript types
 */
export type NamesToType<T extends readonly Type[]> = T extends [] ? [] : T extends readonly [infer U extends Type] ? [NameToType<U>] : T extends readonly [infer U extends Type, ...infer Rest extends readonly Type[]] ? [NameToType<U>, ...NamesToType<Rest>] : [];
export type Functions = typeof functions;
export type Wrapped<T extends Record<string, readonly [readonly Type[], Type]>> = {
    [P in keyof T]: CWrappedFunc<T[P][0], T[P][1]>;
};
export type Methods = Wrapped<Functions>;
export type WrappedPdfiumModule = {
    pdfium: PdfiumModule & PdfiumRuntimeMethods;
} & Methods;
export declare function createWrappedModule(pdfium: PdfiumModule & PdfiumRuntimeMethods): Promise<WrappedPdfiumModule>;
