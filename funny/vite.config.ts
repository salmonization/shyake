import { defineConfig } from 'vite-plus';

export default defineConfig({
    server: { port: 5173, host: '127.0.0.1', fs: { allow: ['..'] } },
    build: { target: 'es2022', chunkSizeWarningLimit: 2048 },
    test: { include: ['src/**/*.test.ts'] },
    lint: {
        ignorePatterns: ['dist/**', 'out/**'],
        options: { typeAware: true, typeCheck: true },
    },
    fmt: {
        ignorePatterns: ['dist/**', 'out/**', 'public/**'],
        tabWidth: 4,
        printWidth: 100,
        singleQuote: true,
        semi: true,
    },
});
