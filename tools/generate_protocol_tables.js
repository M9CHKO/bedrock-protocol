#!/usr/bin/env node
'use strict'

const fs = require('fs')
const path = require('path')

function die (msg) {
  console.error('[GEN ERROR]', msg)
  process.exit(1)
}

function exists (p) {
  try { return fs.existsSync(p) } catch { return false }
}

function readJson (p) {
  try {
    return JSON.parse(fs.readFileSync(p, 'utf8'))
  } catch (e) {
    die('failed to read json: ' + p + '\n' + e.stack)
  }
}

function esc (s) {
  return String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"')
}

function ident (version) {
  return String(version).replace(/[^A-Za-z0-9_]/g, '_')
}

function findDataRoot () {
  const argRoot = process.argv[3]
  const candidates = [
    argRoot,
    path.join(process.cwd(), 'data', 'bedrock'),
    path.join(process.cwd(), 'bedrock'),
    path.join(process.cwd(), 'node_modules', 'minecraft-data', 'data', 'bedrock'),
    path.join(process.cwd(), 'node_modules', 'minecraft-data', 'minecraft-data', 'data', 'bedrock'),
    path.join(process.cwd(), '..', 'node_modules', 'minecraft-data', 'minecraft-data', 'data', 'bedrock')
  ].filter(Boolean)

  for (const c of candidates) {
    if (exists(c)) return c
  }

  die('bedrock data root not found. Put bedrock.zip in project root and unzip, or use node_modules/minecraft-data.')
}

function listVersions (root, requested) {
  const protocolVersionsPath = path.join(root, 'common', 'protocolVersions.json')
  const dataPathsPath = path.join(root, '..', 'dataPaths.json')

  // minecraft-data keeps release aliases such as 1.19.63 and 1.20.15 as
  // version-only directories. Their actual protocol/type schema is recorded
  // in dataPaths.json. Walking only directories containing protocol.json
  // silently dropped those public releases from the generated registry.
  if (exists(protocolVersionsPath)) {
    let releases = readJson(protocolVersionsPath)
      .filter(v => v && v.releaseType === 'release')

    if (requested !== 'all') {
      releases = releases.filter(v => v.minecraftVersion === requested)
      if (releases.length === 0) {
        die(`release version ${requested} not found in ${protocolVersionsPath}`)
      }
    }

    const dataPaths = exists(dataPathsPath) ? readJson(dataPathsPath) : null
    return releases.map(entry => {
      const minecraftVersion = entry.minecraftVersion
      let schemaVersion = minecraftVersion
      if (!exists(path.join(root, schemaVersion, 'protocol.json'))) {
        const protocolPath = dataPaths && dataPaths.bedrock &&
          dataPaths.bedrock[minecraftVersion] &&
          dataPaths.bedrock[minecraftVersion].protocol
        if (!protocolPath) {
          die(`protocol schema alias for ${minecraftVersion} not found in ${dataPathsPath}`)
        }
        schemaVersion = protocolPath.split('/').pop()
      }
      if (!exists(path.join(root, schemaVersion, 'protocol.json'))) {
        die(`protocol.json for ${minecraftVersion} (schema ${schemaVersion}) not found in ${root}`)
      }
      return {
        minecraftVersion,
        schemaVersion,
        protocolVersion: Number(entry.version)
      }
    })
  }

  // Fallback for standalone extracted data sets without minecraft-data's
  // common metadata. These cannot describe aliases, but remain useful for a
  // single explicitly requested schema.
  const dirs = fs.readdirSync(root)
    .filter(v => exists(path.join(root, v, 'protocol.json')) && exists(path.join(root, v, 'version.json')))
    .sort((a, b) => a.localeCompare(b, undefined, { numeric: true }))

  const selected = requested === 'all' ? dirs : dirs.filter(v => v === requested)
  if (selected.length === 0) {
    die(`version ${requested} not found in ${root}`)
  }
  return selected.map(minecraftVersion => ({
    minecraftVersion,
    schemaVersion: minecraftVersion,
    protocolVersion: Number(readJson(path.join(root, minecraftVersion, 'version.json')).version || 0)
  }))
}

function parseMcpePacketInfo (protocolJson) {
  const mcpe = protocolJson.types && protocolJson.types.mcpe_packet
  if (!Array.isArray(mcpe) || mcpe[0] !== 'container' || !Array.isArray(mcpe[1])) {
    die('protocol.json has unsupported mcpe_packet layout')
  }

  const fields = mcpe[1]
  const nameField = fields.find(f => f && f.name === 'name')
  const paramsField = fields.find(f => f && f.name === 'params')

  if (!nameField || !Array.isArray(nameField.type) || nameField.type[0] !== 'mapper') {
    die('mcpe_packet.name mapper not found')
  }

  const mappings = nameField.type[1] && nameField.type[1].mappings
  if (!mappings) die('mcpe_packet.name mappings not found')

  let paramsTypes = {}
  if (paramsField && Array.isArray(paramsField.type) && paramsField.type[0] === 'switch') {
    paramsTypes = paramsField.type[1].fields || {}
  }

  return Object.keys(mappings)
    .map(k => ({
      id: Number(k),
      name: mappings[k],
      paramsType: paramsTypes[mappings[k]] || ''
    }))
    .filter(p => Number.isFinite(p.id))
    .sort((a, b) => a.id - b.id)
}

function generate (root, versions) {
  const includeDir = path.join(process.cwd(), 'include', 'bedrock', 'generated')
  const srcDir = path.join(process.cwd(), 'src', 'generated')
  fs.mkdirSync(includeDir, { recursive: true })
  fs.mkdirSync(srcDir, { recursive: true })

  const versionData = []

  for (const versionSpec of versions) {
    const version = versionSpec.minecraftVersion
    const dir = path.join(root, versionSpec.schemaVersion)
    const protocolJson = readJson(path.join(dir, 'protocol.json'))
    const packets = parseMcpePacketInfo(protocolJson)

    versionData.push({
      minecraftVersion: version,
      protocolVersion: versionSpec.protocolVersion,
      schemaVersion: versionSpec.schemaVersion,
      packets
    })

    const alias = versionSpec.schemaVersion === version ? '' : ` schema=${versionSpec.schemaVersion}`
    console.log(`[GEN] ${version}: protocol=${versionSpec.protocolVersion}${alias} packets=${packets.length}`)
  }

  const hpp = `#pragma once

#include <bedrock/protocol/GeneratedProtocolRegistry.hpp>

namespace bedrock {

const ProtocolVersionInfo* generatedProtocolVersionByName(const std::string& minecraftVersion);
std::vector<std::string> generatedProtocolVersionNames();

} // namespace bedrock
`

  let cpp = `#include <bedrock/generated/GeneratedProtocolTables.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace bedrock {

`

  for (const v of versionData) {
    const id = ident(v.minecraftVersion)
    cpp += `static const ProtocolPacketInfo PACKETS_${id}[] = {\n`
    for (const p of v.packets) {
      cpp += `    { ${p.id}u, "${esc(p.name)}", "${esc(p.paramsType)}" },\n`
    }
    cpp += `};\n\n`
    cpp += `static const ProtocolVersionInfo VERSION_${id} = {\n`
    cpp += `    "${esc(v.minecraftVersion)}",\n`
    cpp += `    ${v.protocolVersion}u,\n`
    cpp += `    PACKETS_${id},\n`
    cpp += `    sizeof(PACKETS_${id}) / sizeof(PACKETS_${id}[0])\n`
    cpp += `};\n\n`
  }

  cpp += `static const ProtocolVersionInfo* const ALL_VERSIONS[] = {\n`
  for (const v of versionData) {
    cpp += `    &VERSION_${ident(v.minecraftVersion)},\n`
  }
  cpp += `};\n\n`

  cpp += `const ProtocolVersionInfo* generatedProtocolVersionByName(const std::string& minecraftVersion) {
    for (const ProtocolVersionInfo* version : ALL_VERSIONS) {
        if (version && minecraftVersion == version->minecraftVersion) {
            return version;
        }
    }
    return nullptr;
}

std::vector<std::string> generatedProtocolVersionNames() {
    std::vector<std::string> out;
    for (const ProtocolVersionInfo* version : ALL_VERSIONS) {
        if (version) out.emplace_back(version->minecraftVersion);
    }
    return out;
}

const ProtocolVersionInfo* getGeneratedProtocolVersion(const std::string& minecraftVersion) {
    return generatedProtocolVersionByName(minecraftVersion);
}

std::vector<std::string> getGeneratedProtocolVersions() {
    return generatedProtocolVersionNames();
}

} // namespace bedrock
`

  fs.writeFileSync(path.join(includeDir, 'GeneratedProtocolTables.hpp'), hpp)
  fs.writeFileSync(path.join(srcDir, 'GeneratedProtocolTables.cpp'), cpp)
}

const requested = process.argv[2] || '1.21.100'
const root = findDataRoot()
console.log('[GEN] data root =', root)
generate(root, listVersions(root, requested))
