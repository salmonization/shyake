export function isValidUsername(username: string): boolean {
    const regex = /^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$/;
    return regex.test(username);
}

export function isReservedUsername(
    username: string, reservedList: string
): boolean {
    if (!reservedList) return false;
    const reserved = reservedList.split(',').map((s) =>
        s.trim().toLowerCase());
    return reserved.includes(username.toLowerCase());
}

// Basic Hashcash verifier (v1)
// Format: 1:bits:date:resource:ext:rand:counter
export async function verifyPoW(
    hashcash: string, requiredBits: number = 20
): Promise<boolean> {
    try {
        const parts = hashcash.split(':');
        if (parts.length !== 7) return false;

        const claimedBits = parseInt(parts[1], 10);
        if (claimedBits < requiredBits) return false;

        const encoder = new TextEncoder();
        const data = encoder.encode(hashcash);
        const hashBuffer = await crypto.subtle.digest('SHA-1', data);
        const hashArray = Array.from(new Uint8Array(hashBuffer));

        let binaryStr = '';
        for (let i = 0; i < Math.ceil(requiredBits / 8) + 1; i++) {
            if (hashArray[i] !== undefined) {
                binaryStr += hashArray[i].toString(2).padStart(8, '0');
            }
        }

        return binaryStr.startsWith('0'.repeat(requiredBits));
    } catch (e) {
        return false;
    }
}
