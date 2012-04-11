#!/usr/bin/env ruby

require 'dbus'

class Vym
  def initialize (name)
    @dbus = DBus::SessionBus.instance
    @service = @dbus.service(name)
    @service.introspect
    @main = @service.object('vym')
    @main.introspect
    @main.default_iface = "org.insilmaril.vym.main.adaptor"

    # Use metaprogramming to create methods for commands in vym
    if modelCount > 0
      m= model(1)
      m.default_iface = "org.insilmaril.vym.model.adaptor"
      s=m.listCommands
      @model_commands=s[0].split ","
      @model_commands.each do |c|
        #puts "c: #{c}"
	self.class.send(:define_method, c) do
	  puts "New method #{c} called."
	  m.execute("#{c}  ()")
	end
      end
    end
  end

  def modelCount
    @main.modelCount[0]
  end

  def model (n)
    if modelCount > 0 && n>=0
      @model = @service.object "vymmodel_#{n}"
      @model.default_iface = "org.insilmaril.vym.model.adaptor"
      return @model
    else
      raise "Error: Model #{n} not accessible in #{@instance}!"
    end  
  end

  def show_methods
    puts "Main methods:"
    @main[@main.default_iface].methods.each do |k,v|
      puts "  #{k}"
    end
    if modelCount > 0
      @model= @service.object 'vymmodel_1'
      @model.default_iface = "org.insilmaril.vym.model.adaptor"
      puts "Model methods:"
      @model[@model.default_iface].methods.each do |k,v|
        puts "  #{k}"
      end
    else
      puts "No model!"
    end  
  end
end

class VymManager
  def initialize
    @dbus = DBus::SessionBus.instance
  end

  def running
    @dbus.proxy.ListNames[0].find_all{|item| item =~/org\.insilmaril\.vym/ }
  end

  def show_running
    puts "Running vym instances:\n  #{running.join "\n  "}"
  end

  def find (name)
    list=running
    for i in (0...list.length)
      vym_service=@dbus.service(list.at(i))
      vym_service.introspect
      vym_main_obj = vym_service.object("vym");
      vym_main_obj.introspect

      vym_main_obj.default_iface = "org.insilmaril.vym.main.adaptor"

      if vym_main_obj.getInstanceName[0]==name 
        puts "Found instance named '#{name}': #{list.at(i)}"
	return list.at(i)
      end  
    end
  end
end

vym_mgr=VymManager.new
vym_mgr.show_running

vym=Vym.new(vym_mgr.find('test') )
vym.show_methods
puts "Modelcount: #{vym.modelCount}"

vym.addBranch
vym.addBranchBefore

