<?php
declare(strict_types=1);

header('Content-Type: application/json; charset=utf-8');

$disabled = array_filter(array_map('trim', explode(',', (string) ini_get('disable_functions'))));
$functions = ['exec', 'shell_exec', 'system', 'passthru', 'proc_open', 'popen'];

$available = [];
foreach ($functions as $name) {
    $available[$name] = function_exists($name) && !in_array($name, $disabled, true);
}

$report = [
    'php_version' => PHP_VERSION,
    'sapi' => PHP_SAPI,
    'os' => PHP_OS_FAMILY,
    'server_software' => $_SERVER['SERVER_SOFTWARE'] ?? null,
    'process_functions' => $available,
    'disabled_functions_contains_process_control' => (bool) array_intersect($functions, $disabled),
    'node_environment_visible_to_php' => [
        'PATH' => getenv('PATH') ?: null,
        'NODE_PATH' => getenv('NODE_PATH') ?: null,
    ],
    'warning' => 'This probe does not launch a process. A positive result still does not prove that long-running processes or public ports are permitted.'
];

echo json_encode($report, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), PHP_EOL;
