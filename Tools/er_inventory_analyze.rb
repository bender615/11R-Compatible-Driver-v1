#!/usr/bin/env ruby
# frozen_string_literal: true

require "csv"
require "digest"
require "json"
require "time"

abort "usage: #{$PROGRAM_NAME} INVENTORY_DIRECTORY" unless ARGV.length == 1
root = File.expand_path(ARGV[0])
settings_path = File.join(root, "settings.csv")
abort "missing #{settings_path}" unless File.file?(settings_path)

rows = CSV.read(settings_path, headers: true)
rigs = rows.group_by { |row| [row["bank"], row["program"]] }

instances = {}
rows.each do |row|
  next if row["slot"] == "Global"
  key = [row["bank"], row["program"], row["slot"]]
  instances[key] ||= {
    "bank" => row["bank"], "program" => row["program"],
    "rig_name" => row["rig_name"], "slot" => row["slot"],
    "category_id" => row["category_id"].to_i,
    "category" => row["category"], "effect_id" => row["effect_id"].to_i,
    "effect" => row["effect"],
    "enabled" => row["enabled"] == "" ? nil : row["enabled"] == "1"
  }
end

model_groups = instances.values.group_by do |instance|
  [instance["category_id"], instance["category"],
   instance["effect_id"], instance["effect"]]
end

models = model_groups.map do |key, uses|
  enabled = uses.count { |use| use["enabled"] == true }
  disabled = uses.count { |use| use["enabled"] == false }
  {
    "category_id" => key[0], "category" => key[1],
    "effect_id" => key[2], "effect" => key[3],
    "instances" => uses.length, "enabled" => enabled,
    "disabled" => disabled, "state_unknown" => uses.length - enabled - disabled,
    "factory_instances" => uses.count { |use| use["bank"] == "factory" },
    "user_instances" => uses.count { |use| use["bank"] == "user" }
  }
end.sort_by { |model| [model["category_id"], model["effect_id"]] }

setting_lookup = rows.each_with_object({}) do |row, lookup|
  lookup[[row["bank"], row["program"], row["slot"], row["key"]]] = row["raw"].to_i
end

def component_rows(rows, key, state_key, setting_lookup)
  rows.select { |row| row["key"] == key }.group_by do |row|
    [row["raw"].to_i, row["display"]]
  end.map do |identity, uses|
    states = uses.map do |use|
      next nil unless state_key
      bypass = setting_lookup[[use["bank"], use["program"], use["slot"], state_key]]
      bypass.nil? ? nil : bypass.zero?
    end
    {
      "raw" => identity[0], "name" => identity[1], "instances" => uses.length,
      "factory_instances" => uses.count { |use| use["bank"] == "factory" },
      "user_instances" => uses.count { |use| use["bank"] == "user" },
      "enabled" => states.count(true),
      "disabled" => states.count(false)
    }
  end.sort_by { |component| [component["name"], component["raw"]] }
end

amps = component_rows(rows, "sld6", "sld5", setting_lookup)
cabs = component_rows(rows, "sldK", "sldJ", setting_lookup)
mics = component_rows(rows, "sldL", nil, setting_lookup)
mic_axes = component_rows(rows, "sldM", nil, setting_lookup)

parameter_groups = rows.group_by do |row|
  [row["effect_id"], row["effect"], row["key"], row["parameter"]]
end
parameters = parameter_groups.map do |key, uses|
  raw_values = uses.map { |use| use["raw"].to_i }
  displays = uses.map { |use| use["display"] }.uniq
  {
    "effect_id" => key[0] == "" ? nil : key[0].to_i,
    "effect" => key[1], "key" => key[2], "parameter" => key[3],
    "observations" => uses.length, "unique_raw_values" => raw_values.uniq.length,
    "minimum_raw" => raw_values.min, "maximum_raw" => raw_values.max,
    "unit" => uses.first["unit"], "range" => uses.first["range"],
    "curve" => uses.first["curve"],
    "display_samples" => displays.first(8)
  }
end.sort_by do |parameter|
  [parameter["effect_id"] || -1, parameter["key"]]
end

fingerprints = {}
manifest_rows = []
%w[factory user].each do |bank|
  (1..26).each do |number|
    %w[A B C D].each do |letter|
      program = format("%02d%s", number, letter)
      path = File.join(root, "#{bank}_#{program}.syx")
      fingerprints[[bank, program]] = Digest::SHA256.file(path).hexdigest
      rig_name = rigs.fetch([bank, program]).first["rig_name"]
      manifest_rows << [bank, program, rig_name, File.size(path),
                        File.mtime(path).iso8601,
                        fingerprints[[bank, program]]]
    end
  end
end

CSV.open(File.join(root, "manifest.csv"), "w") do |csv|
  csv << %w[bank program rig_name bytes captured_at sha256]
  manifest_rows.each { |row| csv << row }
end
File.write(File.join(root, "SHA256SUMS"),
           manifest_rows.map { |row| "#{row[5]}  #{row[0]}_#{row[1]}.syx" }.join("\n") + "\n")

same_program_matches = fingerprints.keys.select do |bank, program|
  bank == "factory" && fingerprints[["factory", program]] == fingerprints[["user", program]]
end.map(&:last)
changed_programs = (fingerprints.keys.map(&:last).uniq - same_program_matches).sort
unique_payloads = fingerprints.values.uniq.length

models_csv = File.join(root, "models.csv")
CSV.open(models_csv, "w") do |csv|
  columns = %w[category_id category effect_id effect instances enabled disabled state_unknown factory_instances user_instances]
  csv << columns
  models.each { |model| csv << model.values_at(*columns) }
end

components_csv = File.join(root, "hardware_components.csv")
CSV.open(components_csv, "w") do |csv|
  csv << %w[type raw name instances factory_instances user_instances enabled disabled]
  { "amp" => amps, "cab" => cabs, "mic" => mics, "mic_axis" => mic_axes }.each do |type, components|
    components.each { |component| csv << [type, *component.values_at("raw", "name", "instances", "factory_instances", "user_instances", "enabled", "disabled")] }
  end
end

parameters_csv = File.join(root, "parameter_catalog.csv")
CSV.open(parameters_csv, "w") do |csv|
  csv << %w[effect_id effect key parameter unit range curve observations unique_raw_values minimum_raw maximum_raw display_samples]
  parameters.each do |parameter|
    csv << parameter.values_at("effect_id", "effect", "key", "parameter", "unit", "range", "curve", "observations", "unique_raw_values", "minimum_raw", "maximum_raw") + [parameter["display_samples"].join(" | ")]
  end
end

summary = {
  "rigs" => rigs.length,
  "settings" => rows.length,
  "slot_instances" => instances.length,
  "unique_payloads" => unique_payloads,
  "factory_user_exact_matches" => same_program_matches.length,
  "factory_user_changed_programs" => changed_programs.length,
  "models" => models,
  "amps" => amps,
  "cabs" => cabs,
  "mics" => mics,
  "mic_axes" => mic_axes,
  "parameters" => parameters
}
File.write(File.join(root, "inventory_summary.json"), JSON.pretty_generate(summary) + "\n")

report = File.join(root, "HardwareBehaviorReport.md")
File.open(report, "w") do |file|
  file.puts "# Eleven Rack Hardware Inventory"
  file.puts
  file.puts "Generated from direct USB-MIDI edit-buffer reads of all 104 Factory and all 104 User programs. Program changes were paced at a minimum of 15 seconds."
  file.puts
  file.puts "## Coverage"
  file.puts
  file.puts "- Rigs captured: **#{rigs.length}/208**"
  file.puts "- Individual setting observations: **#{rows.length}**"
  file.puts "- Slot/module instances: **#{instances.length}**"
  file.puts "- Unique complete rig payloads: **#{unique_payloads}**"
  file.puts "- Same-address Factory/User exact matches: **#{same_program_matches.length}/104**"
  file.puts "- Same-address Factory/User differences: **#{changed_programs.length}/104**"
  file.puts
  file.puts "Exact Factory/User matches: #{same_program_matches.join(', ')}"
  file.puts
  file.puts "## Amplifiers"
  file.puts
  file.puts "| Raw ID | Amp | Uses | Factory | User | Enabled | Disabled |"
  file.puts "|---:|---|---:|---:|---:|---:|---:|"
  amps.each { |amp| file.puts "| #{amp['raw']} | #{amp['name']} | #{amp['instances']} | #{amp['factory_instances']} | #{amp['user_instances']} | #{amp['enabled']} | #{amp['disabled']} |" }
  file.puts
  file.puts "## Cabinets"
  file.puts
  file.puts "| ID | Cabinet | Uses | Factory | User | Enabled | Bypassed |"
  file.puts "|---:|---|---:|---:|---:|---:|---:|"
  cabs.each { |cab| file.puts "| #{cab['raw']} | #{cab['name']} | #{cab['instances']} | #{cab['factory_instances']} | #{cab['user_instances']} | #{cab['enabled']} | #{cab['disabled']} |" }
  file.puts
  file.puts "## Microphones"
  file.puts
  file.puts "| ID | Microphone | Uses | Factory | User |"
  file.puts "|---:|---|---:|---:|---:|"
  mics.each { |mic| file.puts "| #{mic['raw']} | #{mic['name']} | #{mic['instances']} | #{mic['factory_instances']} | #{mic['user_instances']} |" }
  file.puts
  file.puts "Mic-axis observations: #{mic_axes.map { |axis| "#{axis['name']}=#{axis['instances']}" }.join(', ')}."
  file.puts
  file.puts "## Effect and Module Models"
  file.puts
  file.puts "| Category | Effect ID | Model | Uses | Enabled | Disabled | Factory | User |"
  file.puts "|---|---:|---|---:|---:|---:|---:|---:|"
  models.each do |model|
    file.puts "| #{model['category']} | #{model['effect_id']} | #{model['effect']} | #{model['instances']} | #{model['enabled']} | #{model['disabled']} | #{model['factory_instances']} | #{model['user_instances']} |"
  end
  file.puts
  file.puts "## Parameter Catalog"
  file.puts
  file.puts "The complete long-form observations are in `settings.csv`; the condensed range/value catalog is in `parameter_catalog.csv`. Each known physical control includes its unit, display range, and linear/log curve. `unverified` entries preserve the normalized value until an exact hardware value string or endpoint is available."
  file.puts
  file.puts "| Effect ID | Model | Key | Parameter | Unit | Display range | Curve | Observations | Unique | Raw minimum | Raw maximum |"
  file.puts "|---:|---|---|---|---|---|---|---:|---:|---:|---:|"
  parameters.each do |parameter|
    file.puts "| #{parameter['effect_id']} | #{parameter['effect']} | `#{parameter['key']}` | #{parameter['parameter']} | #{parameter['unit']} | #{parameter['range']} | #{parameter['curve']} | #{parameter['observations']} | #{parameter['unique_raw_values']} | #{parameter['minimum_raw']} | #{parameter['maximum_raw']} |"
  end
end

puts "analyzed #{rigs.length} rigs / #{rows.length} settings"
puts "models=#{models.length} amps=#{amps.length} cabs=#{cabs.length} mics=#{mics.length} unique_payloads=#{unique_payloads}"
puts "factory_user_matches=#{same_program_matches.length} changed=#{changed_programs.length}"
puts "report: #{report}"
