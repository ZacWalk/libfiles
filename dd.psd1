@{
    schema = 1
    project = @{
        name = 'libfiles'
        type = 'library'
        'default-target' = 'libfiles'
    }
    dependencies = @{ owner = 'dd' }
    build = @{
        'x64-windows' = @{ debug = 'windows-debug'; release = 'windows-release' }
        'x64-linux' = @{ debug = 'linux-debug'; release = 'linux-release' }
    }
    targets = @(
        @{
            id = 'libfiles'
            kind = 'library'
            'cmake-target' = 'libfiles'
            'test-label' = 'libfiles'
            'debug-path' = 'build/{platform}/debug/{libprefix}files{lib}'
            'release-path' = 'build/{platform}/release/{libprefix}files{lib}'
        }
    )
}
