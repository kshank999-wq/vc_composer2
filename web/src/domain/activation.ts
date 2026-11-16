import { isActivationSlotAvailable, licenseLive, seatLimit } from './commerce';
import type { DeviceActivation, License } from './commerce';
import type { Platform } from './common';

/**
 * Licence activation and device management.
 *
 * The requirement that shapes this file: support must never need a manual
 * database edit. Everything a customer might need (seeing their devices,
 * freeing a seat from a computer they no longer have, re-activating after a
 * reinstall) is reachable by the customer themselves.
 *
 * A deactivated activation is kept, not deleted, so support can answer "what
 * happened on this licence" from the record instead of guessing.
 */

export type ActivationOutcome =
  | { result: 'activated'; reason: 'new_device' }
  | { result: 'activated'; reason: 'already_active' }
  | { result: 'activated'; reason: 'reactivated' }
  | { result: 'refused'; reason: 'license_inactive' }
  | { result: 'refused'; reason: 'no_slots'; inUse: number; limit: number };

export interface ActivationRequest {
  license: License;
  activations: readonly DeviceActivation[];
  deviceFingerprint: string;
  platform: Platform;
  /** When this is being asked. A lapse is a fact about a date, so tests pass one. */
  now?: Date;
}

/**
 * Decide what activating this device on this licence should do.
 *
 * A device that is already active is a no-op (reinstalling must not burn a
 * second seat), a device that was previously deactivated reclaims its own
 * record rather than creating a duplicate, and a new device takes a free slot.
 */
export const decideActivation = (request: ActivationRequest): ActivationOutcome => {
  // `licenseLive` reads the paid period as well as the status: a subscription
  // that has run out may still say active if Stripe's cancellation never
  // arrived. This refuses a new machine; it touches nothing on the old ones.
  if (!licenseLive(request.license, request.now ?? new Date())) {
    return { result: 'refused', reason: 'license_inactive' };
  }

  const own = request.activations.filter((activation) => activation.licenseId === request.license.id);
  const existing = own.find((activation) => activation.deviceFingerprint === request.deviceFingerprint);

  if (existing && existing.deactivatedAt === null) {
    return { result: 'activated', reason: 'already_active' };
  }

  const active = own.filter((activation) => activation.deactivatedAt === null);
  if (existing) {
    return isActivationSlotAvailable(request.license, active)
      ? { result: 'activated', reason: 'reactivated' }
      : { result: 'refused', reason: 'no_slots', inUse: active.length, limit: request.license.maxActivations };
  }

  return isActivationSlotAvailable(request.license, active)
    ? { result: 'activated', reason: 'new_device' }
    : { result: 'refused', reason: 'no_slots', inUse: active.length, limit: request.license.maxActivations };
};

export interface ActivationSummary {
  active: DeviceActivation[];
  past: DeviceActivation[];
  inUse: number;
  /** Null where the licence covers any number of computers. */
  limit: number | null;
  slotsFree: number | null;
}

/** What the account page shows: what is using a seat, and what used to. */
export const summariseActivations = (
  license: License,
  activations: readonly DeviceActivation[],
): ActivationSummary => {
  const own = activations.filter((activation) => activation.licenseId === license.id);
  const active = own.filter((activation) => activation.deactivatedAt === null);
  const past = own.filter((activation) => activation.deactivatedAt !== null);

  const limit = seatLimit(license);
  return {
    active,
    past,
    inUse: active.length,
    limit,
    slotsFree: limit === null ? null : Math.max(0, limit - active.length),
  };
};

/**
 * A readable name for a device the customer has to recognise in a list, since
 * freeing the right seat depends on telling one computer from another.
 */
export const describeDevice = (activation: DeviceActivation): string => {
  const name = activation.deviceName.trim();
  const platform = activation.platform === 'windows' ? 'Windows' : 'Mac';
  if (name.length > 0) return `${name} (${platform})`;
  // Fingerprints are opaque; a short tail is enough to tell two apart.
  return `Unnamed ${platform} · …${activation.deviceFingerprint.slice(-6)}`;
};

/** Human-readable reason for a refusal, suitable for showing to a customer. */
export const explainRefusal = (outcome: ActivationOutcome): string | null => {
  if (outcome.result !== 'refused') return null;
  if (outcome.reason === 'license_inactive') {
    return 'This licence is no longer active. If that is unexpected, check your account.';
  }
  return `This licence is on ${outcome.inUse} of ${outcome.limit} computers. Free a seat from My account and try again.`;
};
