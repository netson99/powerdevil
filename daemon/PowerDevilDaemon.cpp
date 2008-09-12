/***************************************************************************
 *   Copyright (C) 2008 by Dario Freddi <drf@kdemod.ath.cx>                *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************
 *                                                                         *
 *   Part of the code in this file was taken from KDE4Powersave and/or     *
 *   Lithium, where noticed.                                               *
 *                                                                         *
 **************************************************************************/

#include "PowerDevilDaemon.h"

#include <kdemacros.h>
#include <KAboutData>
#include <KPluginFactory>
#include <KNotification>
#include <KIcon>
#include <KMessageBox>
#include <kpluginfactory.h>
#include <klocalizedstring.h>
#include <kjob.h>
#include <kworkspace/kdisplaymanager.h>

#include <QWidget>
#include <QTimer>

#include "PowerDevilSettings.h"
#include "powerdeviladaptor.h"

#include <solid/device.h>
#include <solid/deviceinterface.h>
#include <solid/processor.h>

#include <config-powerdevil.h>

K_PLUGIN_FACTORY( PowerDevilFactory,
                  registerPlugin<PowerDevilDaemon>(); )
K_EXPORT_PLUGIN( PowerDevilFactory( "powerdevildaemon" ) )

PowerDevilDaemon::PowerDevilDaemon( QObject *parent, const QList<QVariant>& )
        : KDEDModule( parent ),
        m_notifier( Solid::Control::PowerManager::notifier() ),
        m_battery( 0 ),
        m_displayManager( new KDisplayManager() ),
        m_pollTimer( new QTimer( this ) ),
        m_currentConfig( 0 )
{
    KGlobal::locale()->insertCatalog( "powerdevil" );

    KAboutData aboutData( "powerdevil", "powerdevil", ki18n( "PowerDevil" ),
                          POWERDEVIL_VERSION, ki18n( "A Power Management tool for KDE4" ),
                          KAboutData::License_GPL, ki18n( "(c) 2008 PowerDevil Development Team" ),
                          KLocalizedString(), "http://www.kde.org" );

    aboutData.addAuthor( ki18n( "Dario Freddi" ), ki18n( "Developer" ), "drf@kdemod.ath.cx",
                         "http://drfav.wordpress.com" );

    m_applicationData = KComponentData( aboutData );

    /* First things first: PowerDevil might be used when another powermanager is already
     * on. So we need to check the system bus and see if another known powermanager has
     * already been registered. Let's see.
     */

    QDBusConnection conn = QDBusConnection::systemBus();

    if ( conn.interface()->isServiceRegistered( "org.freedesktop.PowerManagement" ) ||
            conn.interface()->isServiceRegistered( "com.novell.powersave" ) ||
            conn.interface()->isServiceRegistered( "org.freedesktop.Policy.Power" ) ||
            conn.interface()->isServiceRegistered( "org.kde.powerdevilsystem" ) ) {
        if ( KMessageBox::warningYesNo( 0, i18n( "Another power manager has been detected. "
                                        "Do you want to start PowerDevil anyway? Please note this may lead to "
                                        "conflicts and undefined behaviour." ),
                                        i18n( "PowerDevil Startup" ) ) == KMessageBox::No ) {
            KMessageBox::information( 0, i18n( "If you do not wish to use PowerDevil as your main power manager, "
                                               "please consider disabling its service in systemsettings. This message "
                                               "will not be shown again once PowerDevil has been disabled." ),
                                      i18n( "PowerDevil" ) );

            return;
        } else {
            KMessageBox::information( 0, i18n( "If you wish to use PowerDevil as your main power manager, please "
                                               "consider disabling other power managers you have enabled on "
                                               "this system. This message will not be shown again once all other "
                                               "power managers have been disabled." ),
                                      i18n( "PowerDevil" ) );
        }
    }

    m_profilesConfig = new KConfig( "powerdevilprofilesrc", KConfig::SimpleConfig );
    setAvailableProfiles( m_profilesConfig->groupList() );

    /* You'll see some switches on m_battery. This is fundamental since PowerDevil might run
     * also on system without batteries. Most of modern desktop systems support CPU scaling,
     * so somebody might find PowerDevil handy, and we don't want it to crash on them. To put it
     * short, we simply bypass all adaptor and battery events if no batteries are found.
     */

    // Here we get our battery interface, it will be useful later.
    foreach( const Solid::Device &device, Solid::Device::listFromType( Solid::DeviceInterface::Battery, QString() ) ) {
        Solid::Device d = device;
        m_battery = qobject_cast<Solid::Battery*> ( d.asDeviceInterface( Solid::DeviceInterface::Battery ) );
    }

    m_screenSaverIface = new OrgFreedesktopScreenSaverInterface( "org.freedesktop.ScreenSaver", "/ScreenSaver",
            QDBusConnection::sessionBus(), this );

    connect( m_screenSaverIface, SIGNAL( activeChanged( bool ) ), SLOT( screensaverActivated( bool ) ) );
    connect( m_notifier, SIGNAL( buttonPressed( int ) ), this, SLOT( buttonPressed( int ) ) );

    /* Those slots are relevant only if we're on a system that has a battery. If not, we simply don't care
     * about them.
     */
    if ( m_battery ) {
        connect( m_notifier, SIGNAL( acAdapterStateChanged( int ) ), this, SLOT( acAdapterStateChanged( int ) ) );

        if ( !connect( m_battery, SIGNAL( chargePercentChanged( int, const QString & ) ), this,
                       SLOT( batteryChargePercentChanged( int, const QString & ) ) ) ) {
            emitCriticalNotification( "powerdevilerror", i18n( "Could not connect to battery interface!\n"
                                      "Please check your system configuration" ) );
        }
    }

    //setup idle timer, with some smart polling
    connect( m_pollTimer, SIGNAL( timeout() ), this, SLOT( poll() ) );

    // This code was taken from Lithium/KDE4Powersave
    m_grabber = new QWidget( 0, Qt::X11BypassWindowManagerHint );
    m_grabber->move( -1000, -1000 );
    m_grabber->setMouseTracking( true );
    m_grabber->installEventFilter( this );
    m_grabber->setObjectName( "PowerDevilGrabberWidget" );

    //DBus
    new PowerDevilAdaptor( this );

    // This gets registered to avoid double copies.
    conn.registerService( "org.kde.powerdevilsystem" );

    // All systems up Houston, let's go!
    refreshStatus();
}

PowerDevilDaemon::~PowerDevilDaemon()
{
    delete m_profilesConfig;
    delete m_displayManager;

    if ( m_currentConfig )
        delete m_currentConfig;
}

bool PowerDevilDaemon::eventFilter( QObject * object, QEvent * event )
{
    if ( object == m_grabber
            && ( event->type() == QEvent::MouseMove || event->type() == QEvent::KeyPress ) ) {
        detectedActivity();
    } else if ( object != m_grabber ) {
        // If it's not the grabber, fallback to default event filter
        return KDEDModule::eventFilter( object, event );
    }

    // Otherwise, simply ignore it
    return false;

}

void PowerDevilDaemon::detectedActivity()
{
    // This code was taken from Lithium/KDE4Powersave

    emit pollEvent( i18n( "Detected Activity" ) );

    releaseAndSetBrightness();

    poll();
}

void PowerDevilDaemon::releaseInputLock()
{
    emit pollEvent( I18N_NOOP( "Grabber widget is off" ) );

    m_grabber->releaseMouse();
    m_grabber->releaseKeyboard();
    m_grabber->hide();
}

void PowerDevilDaemon::releaseAndSetBrightness()
{
    KConfigGroup * settings = getCurrentProfile();

    Solid::Control::PowerManager::setBrightness( settings->readEntry( "brightness" ).toInt() );

    releaseInputLock();
}

void PowerDevilDaemon::waitForActivity()
{
    // This code was taken from Lithium/KDE4Powersave

    emit pollEvent( I18N_NOOP( "Grabber widget is on" ) );

    m_grabber->show();
    m_grabber->grabMouse();
    m_grabber->grabKeyboard();

}

void PowerDevilDaemon::screensaverActivated( bool activated )
{
    // We care only if it has been disactivated

    if ( !activated ) {
        m_screenSaverIface->SimulateUserActivity();
        releaseAndSetBrightness();
        poll();
    }
}

void PowerDevilDaemon::refreshStatus()
{
    /* The configuration could have changed if this function was called, so
     * let's resync it.
     */
    PowerDevilSettings::self()->readConfig();
    m_profilesConfig->reparseConfiguration();

    reloadProfile();

    getCurrentProfile( true );

    /* Let's force status update, if we have a battery. Otherwise, let's just
     * re-apply the current profile.
     */
    if ( m_battery ) {
        acAdapterStateChanged( Solid::Control::PowerManager::acAdapterState(), true );
    } else {
        applyProfile();
    }
}

void PowerDevilDaemon::acAdapterStateChanged( int state, bool forced )
{
    if ( state == Solid::Control::PowerManager::Plugged && !forced ) {
        setACPlugged( true );
        emitNotification( "pluggedin", i18n( "The power adaptor has been plugged in" ) );
    }

    if ( state == Solid::Control::PowerManager::Unplugged && !forced ) {
        setACPlugged( false );
        emitNotification( "unplugged", i18n( "The power adaptor has been unplugged" ) );
    }

    if ( !forced )
        reloadProfile( state );

    applyProfile();
}

void PowerDevilDaemon::applyProfile()
{
    KConfigGroup * settings = getCurrentProfile();

    if ( !settings )
        return;

    Solid::Control::PowerManager::setBrightness( settings->readEntry( "brightness" ).toInt() );
    Solid::Control::PowerManager::setCpuFreqPolicy(( Solid::Control::PowerManager::CpuFreqPolicy )
            settings->readEntry( "cpuPolicy" ).toInt() );

    QVariant var = settings->readEntry( "disabledCPUs", QVariant() );
    QList<QVariant> list = var.toList();

    foreach( const Solid::Device &device, Solid::Device::listFromType( Solid::DeviceInterface::Processor, QString() ) ) {
        Solid::Device d = device;
        Solid::Processor * processor = qobject_cast<Solid::Processor * > ( d.asDeviceInterface( Solid::DeviceInterface::Processor ) );

        bool enable = true;

        foreach( const QVariant &ent, list ) {
            if ( processor->number() == ent.toInt() ) {
                enable = false;
            }
        }

        Solid::Control::PowerManager::setCpuEnabled( processor->number(), enable );
    }

    Solid::Control::PowerManager::setScheme( settings->readEntry( "scheme" ) );

    poll();
}

void PowerDevilDaemon::batteryChargePercentChanged( int percent, const QString &udi )
{
    Q_UNUSED( udi )

    setBatteryPercent( percent );

    if ( Solid::Control::PowerManager::acAdapterState() == Solid::Control::PowerManager::Plugged )
        return;

    if ( percent <= PowerDevilSettings::batteryCriticalLevel() ) {
        switch ( PowerDevilSettings::batLowAction() ) {
        case Shutdown:
            emitWarningNotification( "criticalbattery", i18n( "Your battery has reached "
                                     "critical level, the PC will now be halted..." ) );
            shutdown();
            break;
        case S2Disk:
            emitWarningNotification( "criticalbattery", i18n( "Your battery has reached "
                                     "critical level, the PC will now be suspended to disk..." ) );
            suspendToDisk();
            break;
        case S2Ram:
            emitWarningNotification( "criticalbattery", i18n( "Your battery has reached "
                                     "critical level, the PC will now be suspended to RAM..." ) );
            suspendToRam();
            break;
        case Standby:
            emitWarningNotification( "criticalbattery", i18n( "Your battery has reached "
                                     "critical level, the PC is now going Standby..." ) );
            standby();
            break;
        default:
            emitWarningNotification( "criticalbattery", i18n( "Your battery has reached "
                                     "critical level, save your work as soon as possible!" ) );
            break;
        }
    } else if ( percent == PowerDevilSettings::batteryWarningLevel() ) {
        emitWarningNotification( "warningbattery", i18n( "Your battery has reached warning level" ) );
        refreshStatus();
    } else if ( percent == PowerDevilSettings::batteryLowLevel() ) {
        emitWarningNotification( "lowbattery", i18n( "Your battery has reached low level" ) );
        refreshStatus();
    }
}

void PowerDevilDaemon::buttonPressed( int but )
{
    if ( but == Solid::Control::PowerManager::LidClose ) {

        KConfigGroup * settings = getCurrentProfile();

        if ( !settings )
            return;

        switch ( settings->readEntry( "lidAction" ).toInt() ) {
        case Shutdown:
            releaseAndSetBrightness();
            shutdown();
            break;
        case S2Disk:
            releaseAndSetBrightness();
            suspendToDisk();
            break;
        case S2Ram:
            releaseAndSetBrightness();
            suspendToRam();
            break;
        case Standby:
            releaseAndSetBrightness();
            standby();
            break;
        case Lock:
            lockScreen();
            break;
        default:
            break;
        }
    }
}

void PowerDevilDaemon::decreaseBrightness()
{
    int currentBrightness = Solid::Control::PowerManager::brightness() - 10;

    if ( currentBrightness < 0 ) {
        currentBrightness = 0;
    }

    Solid::Control::PowerManager::setBrightness( currentBrightness );
}

void PowerDevilDaemon::increaseBrightness()
{
    int currentBrightness = Solid::Control::PowerManager::brightness() + 10;

    if ( currentBrightness > 100 ) {
        currentBrightness = 100;
    }

    Solid::Control::PowerManager::setBrightness( currentBrightness );
}

void PowerDevilDaemon::shutdown()
{
    emitNotification( "doingjob", i18n( "The computer is being halted" ) );
    m_displayManager->shutdown( KWorkSpace::ShutdownTypeHalt, KWorkSpace::ShutdownModeTryNow );
}

void PowerDevilDaemon::suspendToDisk()
{
    m_screenSaverIface->SimulateUserActivity(); //prevent infinite suspension loops

    if ( PowerDevilSettings::configLockScreen() ) {
        lockScreen();
    }

    emitNotification( "doingjob", i18n( "The computer will now be suspended to disk" ) );

    KJob * job = Solid::Control::PowerManager::suspend( Solid::Control::PowerManager::ToDisk );
    connect( job, SIGNAL( result( KJob * ) ), this, SLOT( suspendJobResult( KJob * ) ) );
    job->start();
}

void PowerDevilDaemon::suspendToRam()
{
    m_screenSaverIface->SimulateUserActivity(); //prevent infinite suspension loops

    if ( PowerDevilSettings::configLockScreen() ) {
        lockScreen();
    }

    emitNotification( "doingjob", i18n( "The computer will now be suspended to RAM" ) );

    KJob * job = Solid::Control::PowerManager::suspend( Solid::Control::PowerManager::ToRam );
    connect( job, SIGNAL( result( KJob * ) ), this, SLOT( suspendJobResult( KJob * ) ) );
    job->start();
}

void PowerDevilDaemon::standby()
{
    m_screenSaverIface->SimulateUserActivity(); //prevent infinite suspension loops

    if ( PowerDevilSettings::configLockScreen() ) {
        lockScreen();
    }

    emitNotification( "doingjob", i18n( "The computer will now be put into standby" ) );

    KJob * job = Solid::Control::PowerManager::suspend( Solid::Control::PowerManager::Standby );
    connect( job, SIGNAL( result( KJob * ) ), this, SLOT( suspendJobResult( KJob * ) ) );
    job->start();
}

void PowerDevilDaemon::suspendJobResult( KJob * job )
{
    if ( job->error() ) {
        emitCriticalNotification( "joberror", QString( i18n( "There was an error while suspending:" )
                                  + QChar( '\n' ) + job->errorString() ) );
    }

    m_screenSaverIface->SimulateUserActivity(); //prevent infinite suspension loops
}

///HACK yucky
#include <QX11Info>
#include <X11/extensions/scrnsaver.h>

void PowerDevilDaemon::poll()
{
    /* This poll function behaves smartly. In fact, polling happens only
     * on-demand. This function is called on the following situations: loading,
     * profile change, resuming, and upon events.
     * As you can see below, the timer is in fact started with the minimum
     * time from next event.
     * The idea was taken from KDE4Powersave & Lithium, so kudos to them.
     * We make an intensive use of qMin/qMax here to determine the minimum time.
     */

    /* Hack! Since KRunner still doesn't behave properly, the
     * correct way to go doesn't work (yet), and it's this one:
    ------------------------------------------------------------
    int idle = m_screenSaverIface->GetSessionIdleTime();
    ------------------------------------------------------------
    */
    /// In the meanwhile, this X11 hackish way gets its job done.
    //----------------------------------------------------------
    XScreenSaverInfo * mitInfo = 0;
    mitInfo = XScreenSaverAllocInfo();
    XScreenSaverQueryInfo( QX11Info::display(), DefaultRootWindow( QX11Info::display() ), mitInfo );
    int idle = mitInfo->idle / 1000;
    //----------------------------------------------------------

    emit pollEvent( i18n( "Polling started, idle time is %1 seconds", idle ) );

    KConfigGroup * settings = getCurrentProfile();

    if ( !settings ) {
        return;
    }

    if ( !PowerDevilSettings::dimOnIdle() && !settings->readEntry( "turnOffIdle", false ) &&
            settings->readEntry( "idleAction" ).toInt() == None ) {
        emit pollEvent( i18n( "Stopping timer" ) );
        m_pollTimer->stop();
        return;
    }

    int dimOnIdleTime = PowerDevilSettings::dimOnIdleTime() * 60;
    int minDimTime = dimOnIdleTime * 1 / 2;
    int minDimEvent = dimOnIdleTime;

    if ( idle < ( dimOnIdleTime * 3 / 4 ) ) {
        minDimEvent = dimOnIdleTime * 3 / 4;
    }
    if ( idle < ( dimOnIdleTime * 1 / 2 ) ) {
        minDimEvent = dimOnIdleTime * 1 / 2;
    }

    int minTime = settings->readEntry( "idleTime" ).toInt() * 60;

    if ( settings->readEntry( "turnOffIdle", QVariant() ).toBool() ) {
        minTime = qMin( minTime, settings->readEntry( "turnOffIdleTime" ).toInt() * 60 );
    }
    if ( PowerDevilSettings::dimOnIdle() ) {
        minTime = qMin( minTime, minDimTime );
    }

    emit pollEvent( i18n( "Minimum time is %1 seconds", minTime ) );

    if ( idle < minTime ) {
        int remaining = minTime - idle;
        m_pollTimer->start( remaining * 1000 );
        emit pollEvent( i18n( "Nothing to do, next event in %1 seconds", remaining ) );
        return;
    }

    /* You'll see we release input lock here sometimes. Why? Well,
     * after some tests, I found out that the offscreen widget doesn't work
     * if the monitor gets turned off or the PC is suspended. But, we don't care
     * about this for a simple reason: the only parameter we need to look into
     * is the brightness, so we just release the lock, set back the brightness
     * to normal, and that's it.
     */

    if ( idle >= settings->readEntry( "idleTime" ).toInt() * 60 ) {
        setUpNextTimeout( idle, minDimEvent );

        switch ( settings->readEntry( "idleAction" ).toInt() ) {
        case Shutdown:
            releaseAndSetBrightness();
            shutdown();
            break;
        case S2Disk:
            releaseAndSetBrightness();
            suspendToDisk();
            break;
        case S2Ram:
            releaseAndSetBrightness();
            suspendToRam();
            break;
        case Standby:
            releaseAndSetBrightness();
            standby();
            break;
        case Lock:
            lockScreen();
            break;
        default:
            waitForActivity();
            break;
        }

        return;

    } else if ( settings->readEntry( "turnOffIdle", QVariant() ).toBool() &&
                ( idle >= ( settings->readEntry( "turnOffIdleTime" ).toInt() * 60 ) ) ) {
        releaseAndSetBrightness();
        turnOffScreen();
    } else if ( PowerDevilSettings::dimOnIdle()
                && ( idle >= dimOnIdleTime ) ) {
        Solid::Control::PowerManager::setBrightness( 0 );
        waitForActivity();
    } else if ( PowerDevilSettings::dimOnIdle()
                && ( idle >= ( dimOnIdleTime * 3 / 4 ) ) ) {
        float newBrightness = Solid::Control::PowerManager::brightness() / 4;
        Solid::Control::PowerManager::setBrightness( newBrightness );
        waitForActivity();
    } else if ( PowerDevilSettings::dimOnIdle() &&
                ( idle >= ( dimOnIdleTime * 1 / 2 ) ) ) {
        float newBrightness = Solid::Control::PowerManager::brightness() / 2;
        Solid::Control::PowerManager::setBrightness( newBrightness );
        waitForActivity();
    } else {
        releaseAndSetBrightness();
    }

    setUpNextTimeout( idle, minDimEvent );
}

void PowerDevilDaemon::setUpNextTimeout( int idle, int minDimEvent )
{
    KConfigGroup *settings = getCurrentProfile();

    int nextTimeout = -1;

    if (( settings->readEntry( "idleTime" ).toInt() * 60 ) > idle ) {
        if ( nextTimeout >= 0 ) {
            nextTimeout = qMin( nextTimeout, ( settings->readEntry( "idleTime" ).toInt() * 60 ) - idle );
        } else {
            nextTimeout = ( settings->readEntry( "idleTime" ).toInt() * 60 ) - idle;
        }
    }
    if (( settings->readEntry( "turnOffIdleTime" ).toInt() * 60 ) > idle &&
            settings->readEntry( "turnOffIdle", QVariant() ).toBool() ) {
        if ( nextTimeout >= 0 ) {
            nextTimeout = qMin( nextTimeout, ( settings->readEntry( "turnOffIdleTime" ).toInt() * 60 ) - idle );
        } else {
            nextTimeout = ( settings->readEntry( "turnOffIdleTime" ).toInt() * 60 ) - idle;
        }
    }
    if ( minDimEvent > idle && PowerDevilSettings::dimOnIdle() ) {
        if ( nextTimeout >= 0 ) {
            nextTimeout = qMin( nextTimeout, minDimEvent - idle );
        } else {
            nextTimeout = minDimEvent - idle;
        }
    }

    if ( nextTimeout >= 0 ) {
        m_pollTimer->start( nextTimeout * 1000 );
        emit pollEvent( i18n( "Next timeout in %1 seconds", nextTimeout ) );
    } else {
        m_pollTimer->stop();
        emit pollEvent( i18n( "Stopping timer" ) );
    }
}

void PowerDevilDaemon::lockScreen()
{
    emitNotification( "doingjob", i18n( "The screen is being locked" ) );
    m_screenSaverIface->Lock();
}

void PowerDevilDaemon::emitCriticalNotification( const QString &evid, const QString &message, const QString &iconname )
{
    /* Those notifications are always displayed */

    KNotification::event( evid, message, KIcon( iconname ).pixmap( 20, 20 ),
                          0, KNotification::CloseOnTimeout, m_applicationData );
}

void PowerDevilDaemon::emitWarningNotification( const QString &evid, const QString &message, const QString &iconname )
{
    if ( !PowerDevilSettings::enableWarningNotifications() )
        return;

    KNotification::event( evid, message, KIcon( iconname ).pixmap( 20, 20 ),
                          0, KNotification::CloseOnTimeout, m_applicationData );
}

void PowerDevilDaemon::emitNotification( const QString &evid, const QString &message, const QString &iconname )
{
    if ( !PowerDevilSettings::enableNotifications() )
        return;

    KNotification::event( evid, message, KIcon( iconname ).pixmap( 20, 20 ),
                          0, KNotification::CloseOnTimeout, m_applicationData );
}

KConfigGroup * PowerDevilDaemon::getCurrentProfile( bool forcereload )
{
    /* We need to access this a lot of times, so we use a cached
     * implementation here. We create the object just if we're sure
     * it is not already valid.
     *
     * IMPORTANT!!! This class already handles deletion of the config
     * object, so you don't have to delete it!!
     */

    bool reload = false;

    if ( !m_currentConfig ) {
        reload = true;
    } else if ( forcereload || m_currentConfig->name() != m_currentProfile ) {
        delete m_currentConfig;
        m_currentConfig = 0;
        reload = true;
    }

    if ( reload ) {
        m_currentConfig = new KConfigGroup( m_profilesConfig, m_currentProfile );
    }

    if ( !m_currentConfig->isValid() || !m_currentConfig->entryMap().size() ) {
        emitCriticalNotification( "powerdevilerror", i18n( "The profile \"%1\" has been selected, "
                                  "but it does not exist!\nPlease check your PowerDevil configuration.",
                                  m_currentProfile ) );
        reloadProfile();
        delete m_currentConfig;
        m_currentConfig = 0;
    }

    return m_currentConfig;
}

void PowerDevilDaemon::reloadProfile( int state )
{
    if ( !m_battery ) {
        setCurrentProfile( PowerDevilSettings::aCProfile() );
    } else {
        if ( state == -1 )
            state = Solid::Control::PowerManager::acAdapterState();

        if ( state == Solid::Control::PowerManager::Plugged ) {
            setCurrentProfile( PowerDevilSettings::aCProfile() );
        } else if ( m_battery->chargePercent() <= PowerDevilSettings::batteryWarningLevel() ) {
            setCurrentProfile( PowerDevilSettings::warningProfile() );
        } else if ( m_battery->chargePercent() <= PowerDevilSettings::batteryLowLevel() ) {
            setCurrentProfile( PowerDevilSettings::lowProfile() );
        } else {
            setCurrentProfile( PowerDevilSettings::batteryProfile() );
        }
    }

    if ( m_currentProfile.isEmpty() ) {
        /* Ok, misconfiguration! Well, first things first: if we have some profiles,
         * let's just load the first available one.
         */

        if ( !m_availableProfiles.isEmpty() ) {
            setCurrentProfile( m_availableProfiles.at( 0 ) );
        } else {
            /* In this case, let's fill our profiles file with the most basic
             * performance profile
             */

            KConfigGroup * performance = new KConfigGroup( m_profilesConfig, "Performance" );

            performance->writeEntry( "brightness", 100 );
            performance->writeEntry( "cpuPolicy", ( int ) Solid::Control::PowerManager::Performance );
            performance->writeEntry( "idleAction", 0 );
            performance->writeEntry( "idleTime", 50 );
            performance->writeEntry( "lidAction", 0 );
            performance->writeEntry( "turnOffIdle", false );
            performance->writeEntry( "turnOffIdleTime", 120 );

            performance->sync();

            delete performance;

            setAvailableProfiles( m_profilesConfig->groupList() );

            setCurrentProfile( m_availableProfiles.at( 0 ) );
        }
    }

    poll();
}

void PowerDevilDaemon::setProfile( const QString & profile )
{
    setCurrentProfile( profile );

    /* Don't call refreshStatus() here, since we don't want the predefined profile
     * to be loaded!!
     */

    if ( m_battery ) {
        acAdapterStateChanged( Solid::Control::PowerManager::acAdapterState(), true );
    } else {
        applyProfile();
    }

    KConfigGroup * settings = getCurrentProfile();

    emitNotification( "profileset", i18n( "Profile changed to \"%1\"", profile ) ,
                      settings->readEntry( "iconname" ) );
}

void PowerDevilDaemon::reloadAndStream()
{
    if ( !m_battery ) {
        setCurrentProfile( PowerDevilSettings::aCProfile() );
        setACPlugged( true );

        setBatteryPercent( 100 );
    } else {
        if ( Solid::Control::PowerManager::acAdapterState() == Solid::Control::PowerManager::Plugged ) {
            setCurrentProfile( PowerDevilSettings::aCProfile() );
            setACPlugged( true );
        } else if ( m_battery->chargePercent() <= PowerDevilSettings::batteryWarningLevel() ) {
            setCurrentProfile( PowerDevilSettings::warningProfile() );
            setACPlugged( false );
        } else if ( m_battery->chargePercent() <= PowerDevilSettings::batteryLowLevel() ) {
            setCurrentProfile( PowerDevilSettings::lowProfile() );
            setACPlugged( false );
        } else {
            setCurrentProfile( PowerDevilSettings::batteryProfile() );
            setACPlugged( false );
        }

        setBatteryPercent( Solid::Control::PowerManager::batteryChargePercent() );
    }

    setAvailableProfiles( m_profilesConfig->groupList() );

    streamData();

    refreshStatus();
}

void PowerDevilDaemon::streamData()
{
    emit profileChanged( m_currentProfile, m_availableProfiles );
    emit stateChanged( m_batteryPercent, m_isPlugged );
}

QStringList PowerDevilDaemon::getSupportedGovernors()
{
    QStringList retlist;

    Solid::Control::PowerManager::CpuFreqPolicies policies = Solid::Control::PowerManager::supportedCpuFreqPolicies();

    if ( policies & Solid::Control::PowerManager::Performance ) {
        retlist.append( i18n( "Performance" ) );
    }

    if ( policies & Solid::Control::PowerManager::OnDemand ) {
        retlist.append( i18n( "Dynamic (ondemand)" ) );
    }

    if ( policies & Solid::Control::PowerManager::Conservative ) {
        retlist.append( i18n( "Dynamic (conservative)" ) );
    }

    if ( policies & Solid::Control::PowerManager::Powersave ) {
        retlist.append( i18n( "Powersave" ) );
    }

    if ( policies & Solid::Control::PowerManager::Userspace ) {
        retlist.append( i18n( "Userspace" ) );
    }

    return retlist;
}

QStringList PowerDevilDaemon::getSupportedSchemes()
{
    return Solid::Control::PowerManager::supportedSchemes();
}

void PowerDevilDaemon::setPowersavingScheme( const QString &scheme )
{
    Solid::Control::PowerManager::setScheme( scheme );
}

void PowerDevilDaemon::setGovernor( const QString &governor )
{
    if ( governor == i18n( "Performance" ) ) {
        Solid::Control::PowerManager::setCpuFreqPolicy( Solid::Control::PowerManager::Performance );
    } else if ( governor == i18n( "Dynamic (ondemand)" ) ) {
        Solid::Control::PowerManager::setCpuFreqPolicy( Solid::Control::PowerManager::OnDemand );
    } else if ( governor == i18n( "Dynamic (conservative)" ) ) {
        Solid::Control::PowerManager::setCpuFreqPolicy( Solid::Control::PowerManager::Conservative );
    } else if ( governor == i18n( "Powersave" ) ) {
        Solid::Control::PowerManager::setCpuFreqPolicy( Solid::Control::PowerManager::Powersave );
    } else if ( governor == i18n( "Userspace" ) ) {
        Solid::Control::PowerManager::setCpuFreqPolicy( Solid::Control::PowerManager::Userspace );
    }
}

void PowerDevilDaemon::setBrightness( int value )
{
    if ( value == -2 ) {
        // Then set brightness to half the current brightness.

        float b = Solid::Control::PowerManager::brightness() / 2;
        Solid::Control::PowerManager::setBrightness( b );
        return;
    }

    Solid::Control::PowerManager::setBrightness( value );
}

void PowerDevilDaemon::turnOffScreen()
{
    /* FIXME: This command works, though we can switch to dpms... need some
     * feedback here.
     */
    QProcess::execute( "xset dpms force off" );
}

void PowerDevilDaemon::setBatteryPercent( int newpercent )
{
    m_batteryPercent = newpercent;
    emit stateChanged( m_batteryPercent, m_isPlugged );
}

void PowerDevilDaemon::setACPlugged( bool newplugged )
{
    m_isPlugged = newplugged;
    emit stateChanged( m_batteryPercent, m_isPlugged );
}

void PowerDevilDaemon::setCurrentProfile( const QString &profile )
{
    m_currentProfile = profile;
    emit profileChanged( m_currentProfile, m_availableProfiles );
}

void PowerDevilDaemon::setAvailableProfiles( const QStringList &aProfiles )
{
    m_availableProfiles = aProfiles;
    emit profileChanged( m_currentProfile, m_availableProfiles );
}

#include "PowerDevilDaemon.moc"
