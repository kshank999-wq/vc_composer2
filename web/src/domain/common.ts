import { z } from 'zod';

/**
 * Shared shapes for the commerce domain.
 *
 * VC Composer 2 ships for Windows and macOS. A platform is data rather than a
 * hard-coded pair of branches, so every gate reads it from the same enum.
 */

export type Id<TBrand extends string> = string & { readonly __brand: TBrand };

export type UserId = Id<'User'>;
export type OrderId = Id<'Order'>;
export type LicenseId = Id<'License'>;
export type ReleaseBuildId = Id<'ReleaseBuild'>;
export type DeviceActivationId = Id<'DeviceActivation'>;
export type AnyId = Id<string>;

export const id = <T extends AnyId>(): z.ZodType<T> => z.string().uuid() as unknown as z.ZodType<T>;

export const isoDateTime = () => z.string().datetime({ offset: true });

export const timestamps = {
  createdAt: isoDateTime(),
  updatedAt: isoDateTime(),
};

export const platformSchema = z.enum(['windows', 'macos']);
export type Platform = z.infer<typeof platformSchema>;

/** What each platform is called where a customer reads it. */
export const PLATFORM_LABEL: Record<Platform, string> = {
  windows: 'Windows 10 / 11',
  macos: 'macOS',
};

/** Mint a new identifier of the requested kind. */
export const newId = <T extends AnyId>(): T => globalThis.crypto.randomUUID() as T;
