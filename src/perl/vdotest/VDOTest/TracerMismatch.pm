##
# Tests that Tracer.pm detects mismatches.
#
# $Id$
##
package VDOTest::TracerMismatch;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use YAML;

use Permabit::Assertions qw(
  assertDefined
  assertMinMaxArgs
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::BlockDevice::TestDevice::Managed::Corruptor;
use Permabit::BlockDevice::TestDevice::Managed::Tracer;
use Permabit::Constants qw($KB $SECTOR_SIZE);
use Permabit::FileSystem::Debug;

use base qw(VDOTest::CorruptionBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple block size for writing data
  blockSize               => 4 * $KB,
  # @ple Use tracer on corruptor
  deviceType              => "tracer-corruptor",
  # whether to trace on each sector (1) or each 4k block (8).
  traceSectors            => 8,
);
##

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);

  $self->SUPER::set_up();

  # Make certain that the debugfs is mounted on the test machine.
  Permabit::FileSystem::Debug->new(machine =>
                                    $self->getDevice()->getMachine())->mount();
}

#############################################################################
# Check that Tracer detected mismatches for the corruption produced by the
# Corruptor.
#
# @param  tracer    the tracer to test
# @param  corruptor the underlying corruptor
##
sub _checkTracerResults {
  my ($self, $tracer, $corruptor) = assertNumArgs(3, @_);

  # Run the corruptor's generated blkparse file through its python script and
  # load the yaml results.
  my $corruptorResult = $corruptor->parseBlockParse();
  assertTrue($corruptorResult->{status} == 0,
             "no error from corruptor blkparse file");
  my $corruptorYaml = YAML::Load($corruptorResult->{stdout});

  # Run the tracer's generated blkparse file through the python check script.
  my $tracerResult = $tracer->parseBlockParse();
  my $tracerMismatchYaml = YAML::Load($tracerResult->{stdout});

  # Get the blocks that tracer saw only once and make an existence map from the
  # returned array.
  my $tracerSinglesResult = $tracer->parseBlockParse(undef, 1);
  assertTrue($tracerSinglesResult->{status} == 0,
             "no error from getting tracer singles");
  my $tracerSinglesYaml = YAML::Load($tracerSinglesResult->{stdout});
  my %tracerSinglesHash
    = map { $_ => 1 } @{$tracerSinglesYaml->{"Single sectors"}};

  # Check the found mismatched sector numbers from the tracer against those
  # generated by the corruptor.
  my %corruptHash;
  my @allMismatches = @{$tracerMismatchYaml->{"Mismatches"}};
  foreach my $sector (@{$corruptorYaml->{"Corrupted sectors"}}) {
    # Get the corrupted block's start sector to compare against.
    my $startSector = $sector - ($sector % $self->{traceSectors});
    $corruptHash{$startSector} = ();
  }

  foreach my $sector (keys %corruptHash) {
    if (!exists($tracerSinglesHash{$sector})) {
      my @sectorMismatches = grep { $_->{"sector"} eq $sector } @allMismatches;
      assertTrue(scalar(@sectorMismatches) > 0,
                 "mismatch detected for sector $sector");
      @allMismatches = grep { $_->{"sector"} ne $sector } @allMismatches;
    }
  }
  assertTrue(scalar(@allMismatches) == 0,
             "no non-corrupted mismatches found");

}

#############################################################################
# Returns a slice for performing I/O (write/read) based on the input
# parameters.
#
# @param  tracerBlockCount  tracer size in blocks
# @param  blockSize         block size in bytes
# @param  minBlockCount     lower limit on how many blocks to write/read;
#                             specified to "cover" the frequency of corruption
#                             for the corruption in effect
# @param  maxBlockCount     upper limit on how many blocks to write/read
# @oparam singlePassVerify  if true, the generated slice will perform only
#                             one pass of verification
#
# @return a slice for performing I/O.
##
sub _getIoSlice {
  my ($self, $tracerBlockCount, $blockSize, $minBlockCount, $maxBlockCount,
      $singlePassVerify)
    = assertMinMaxArgs([0], 5, 6, @_);

  my $blockCount
    = $minBlockCount + int(rand($maxBlockCount - $minBlockCount + 1));
  my $blockOffset = int(rand($tracerBlockCount - $blockCount)) + 1;

  return $self->createSlice(blockCount        => $blockCount,
                            blockSize         => $blockSize,
                            offset            => $blockOffset,
                            singlePassVerify  => $singlePassVerify);
}

#############################################################################
# Test that Tracer detects mismatches generated by the Corruptor read modes.
##
sub testMismatchRead {
  my ($self) = assertNumArgs(1, @_);
  my ($tracer, $corruptor) = $self->getDevices();
  my $tracerBlockCount     = $tracer->getSize() / $self->{blockSize};
  my $maxBlockCount        = int($tracerBlockCount / 10);
  if ($maxBlockCount == 0) {
    $maxBlockCount = 1;
  }
  if ($maxBlockCount > 128) {
    $maxBlockCount = 128;
  }
  my $minBlockCount = int($maxBlockCount / 4);
  if ($minBlockCount == 0) {
    $minBlockCount = 1;
  }

  # Modulo read corruption
  # Enable and start block tracing on the tracer instance.
  $tracer->enable();
  $tracer->startBlockTrace();

  # Tracing the corruptor and enabling of corruption takes place after
  # having written as the slice writing involves reading (e.g., blkid) at the
  # corruptor device level and those are invisible to the tracer instance.
  my $slice = $self->_getIoSlice($tracerBlockCount,
                                 $self->{blockSize},
                                 $minBlockCount,
                                 $maxBlockCount);
  $self->_writeSlice($slice, { tag => "direct" });

  # For non-random corruption the verify should fail because the minimum block
  # count "covers" the frequency of corruption.  For random corruption it may
  # not fail.
  $corruptor->startBlockTrace();
  $corruptor->enableModuloRead($minBlockCount
                                * ($self->{blockSize} / $SECTOR_SIZE));
  $self->_verifySliceFailure($slice);

  # Disable corruption and stop block tracing on the corruptor.
  $corruptor->disableCurrentRead();
  $corruptor->stopBlockTrace(1);

  # Stop block tracing and disable it on the tracer instance.
  $tracer->stopBlockTrace(1);
  $tracer->disable();

  # Verify that the trace found everything the corruptor corrupted.
  $self->_checkTracerResults($tracer, $corruptor);

  # Random read corruption
  # Enable and start block tracing on the tracer instance.
  $tracer->enable();
  $tracer->startBlockTrace();

  # Tracing the corruptor and enabling of corruption takes place after
  # having written as the slice writing involves reading (e.g., blkid) at the
  # corruptor device level and those are invisible to the tracer instance.
  $slice = $self->_getIoSlice($tracerBlockCount,
                              $self->{blockSize},
                              $minBlockCount,
                              $maxBlockCount);
  $self->_writeSlice($slice, { tag => "direct" });

  # For non-random corruption the verify should fail because the minimum block
  # count "covers" the frequency of corruption.  For random corruption it may
  # not fail.
  $corruptor->startBlockTrace();
  $corruptor->enableRandomRead($minBlockCount
                                * ($self->{blockSize} / $SECTOR_SIZE));
  $self->_verifySlicePotentialFailure($slice);

  # Disable corruption and stop block tracing on the corruptor.
  $corruptor->disableCurrentRead();
  $corruptor->stopBlockTrace(1);

  # Stop block tracing and disable it on the tracer instance.
  $tracer->stopBlockTrace(1);
  $tracer->disable();

  # Verify that the trace found everything the corruptor corrupted.
  $self->_checkTracerResults($tracer, $corruptor);

  # Sequential read corruption
  # Enable and start block tracing on the tracer instance.
  $tracer->enable();
  $tracer->startBlockTrace();

  # Tracing the corruptor and enabling of corruption takes place after
  # having written as the slice writing involves reading (e.g., blkid) at the
  # corruptor device level and those are invisible to the tracer instance.
  $slice = $self->_getIoSlice($tracerBlockCount,
                              $self->{blockSize},
                              $minBlockCount,
                              $maxBlockCount);
  $self->_writeSlice($slice, { tag => "direct" });

  # For non-random corruption the verify should fail because the minimum block
  # count "covers" the frequency of corruption.  For random corruption it may
  # not fail.
  $corruptor->startBlockTrace();
  $corruptor->enableSequentialRead($minBlockCount
                                    * ($self->{blockSize} / $SECTOR_SIZE));
  $self->_verifySliceFailure($slice);

  # Disable corruption and stop block tracing on the corruptor.
  $corruptor->disableCurrentRead();
  $corruptor->stopBlockTrace(1);

  # Stop block tracing and disable it on the tracer instance.
  $tracer->stopBlockTrace(1);
  $tracer->disable();

  # Verify that the trace found everything the corruptor corrupted.
  $self->_checkTracerResults($tracer, $corruptor);
}

#############################################################################
# Test that Tracer detects mismatches generated by the Corruptor write modes.
##
sub testMismatchWrite {
  my ($self) = assertNumArgs(1, @_);
  my ($tracer, $corruptor) = $self->getDevices();
  my $tracerBlockCount     = $tracer->getSize() / $self->{blockSize};
  my $maxBlockCount        = int($tracerBlockCount / 10);
  if ($maxBlockCount == 0) {
    $maxBlockCount = 1;
  }
  if ($maxBlockCount > 128) {
    $maxBlockCount = 128;
  }
  my $minBlockCount = int($maxBlockCount / 4);
  if ($minBlockCount == 0) {
    $minBlockCount = 1;
  }

  # Modulo write corruption
  # Enable and start block tracing on the tracer instance.
  $tracer->enable();
  $tracer->startBlockTrace();

  # Start tracing the corruptor and then enable corruption.
  $corruptor->startBlockTrace();
  $corruptor->enableModuloWrite($minBlockCount
                                  * ($self->{blockSize} / $SECTOR_SIZE));

  # Perform I/O
  my $slice = $self->_getIoSlice($tracerBlockCount,
                                 $self->{blockSize},
                                 $minBlockCount,
                                 $maxBlockCount);
  $self->_writeSlice($slice, { tag => "direct" });

  # For non-random corruption the verify should fail because the minimum block
  # count "covers" the frequency of corruption.  For random corruption it may
  # not fail.
  $self->_verifySliceFailure($slice);

  # Disable corruption and stop block tracing on the corruptor.
  $corruptor->disableCurrentWrite();
  $corruptor->stopBlockTrace(1);

  # Stop block tracing and disable it on the tracer instance.
  $tracer->stopBlockTrace(1);
  $tracer->disable();

  # Verify that the trace found everything the corruptor corrupted.
  $self->_checkTracerResults($tracer, $corruptor);

  # Random write corruption
  # Enable and start block tracing on the tracer instance.
  $tracer->enable();
  $tracer->startBlockTrace();

  # Start tracing the corruptor and then enable corruption.
  $corruptor->startBlockTrace();
  $corruptor->enableRandomWrite($minBlockCount
                                  * ($self->{blockSize} / $SECTOR_SIZE));

  # Perform I/O
  $slice = $self->_getIoSlice($tracerBlockCount,
                              $self->{blockSize},
                              $minBlockCount,
                              $maxBlockCount);
  $self->_writeSlice($slice, { tag => "direct" });

  # For non-random corruption the verify should fail because the minimum block
  # count "covers" the frequency of corruption.  For random corruption it may
  # not fail.
  $self->_verifySlicePotentialFailure($slice);

  # Disable corruption and stop block tracing on the corruptor.
  $corruptor->disableCurrentWrite();
  $corruptor->stopBlockTrace(1);

  # Stop block tracing and disable it on the tracer instance.
  $tracer->stopBlockTrace(1);
  $tracer->disable();

  # Verify that the trace found everything the corruptor corrupted.
  $self->_checkTracerResults($tracer, $corruptor);

  # Sequential write corruption
  # Enable and start block tracing on the tracer instance.
  $tracer->enable();
  $tracer->startBlockTrace();

  # Start tracing the corruptor and then enable corruption.
  $corruptor->startBlockTrace();
  $corruptor->enableSequentialWrite($minBlockCount
                                      * ($self->{blockSize} / $SECTOR_SIZE));

  # Perform I/O
  $slice = $self->_getIoSlice($tracerBlockCount,
                              $self->{blockSize},
                              $minBlockCount,
                              $maxBlockCount);
  $self->_writeSlice($slice, { tag => "direct" });

  # For non-random corruption the verify should fail because the minimum block
  # count "covers" the frequency of corruption.  For random corruption it may
  # not fail.
  $self->_verifySliceFailure($slice);

  # Disable corruption and stop block tracing on the corruptor.
  $corruptor->disableCurrentWrite();
  $corruptor->stopBlockTrace(1);

  # Stop block tracing and disable it on the tracer instance.
  $tracer->stopBlockTrace(1);
  $tracer->disable();

  # Verify that the trace found everything the corruptor corrupted.
  $self->_checkTracerResults($tracer, $corruptor);
}

#############################################################################
# Test that Tracer correctly handles the situation where corruption happens
# but in such a way that Tracer can't detect it (e.g., all reads with no
# repeats).
##
sub testOnlySingles {
  my ($self) = assertNumArgs(1, @_);
  my ($tracer, $corruptor) = $self->getDevices();
  my $tracerBlockCount     = $tracer->getSize() / $self->{blockSize};
  my $maxBlockCount        = int($tracerBlockCount / 10);
  if ($maxBlockCount == 0) {
    $maxBlockCount = 1;
  }
  if ($maxBlockCount > 128) {
    $maxBlockCount = 128;
  }
  my $minBlockCount = int($maxBlockCount / 4);
  if ($minBlockCount == 0) {
    $minBlockCount = 1;
  }

  # Tracing the corruptor and enabling of corruption takes place after
  # having written as the slice writing involves reading (e.g., blkid) at the
  # corruptor device level and those are invisible to the tracer instance.

  # Have this slice perform only single pass verification.
  my $slice = $self->_getIoSlice($tracerBlockCount,
                                 $self->{blockSize},
                                 $minBlockCount,
                                 $maxBlockCount,
                                 1);
  $self->_writeSlice($slice, { tag => "direct" });

  # Enable and start block tracing on the tracer instance.
  $tracer->enable();
  $tracer->startBlockTrace();

  # For non-random corruption the verify should fail because the minimum block
  # count "covers" the frequency of corruption.
  $corruptor->startBlockTrace();
  $corruptor->enableModuloRead($minBlockCount
                                * ($self->{blockSize} / $SECTOR_SIZE));
  $self->_verifySliceFailure($slice);

  # Disable corruption and stop block tracing on the corruptor.
  $corruptor->disableCurrentRead();
  $corruptor->stopBlockTrace(1);

  # Stop block tracing and disable it on the tracer instance.
  $tracer->stopBlockTrace(1);
  $tracer->disable();

  # Verify that the tracer did not find anything that the corruptor corrupted.
  my $tracerResult = $tracer->parseBlockParse();
  assertTrue($tracerResult->{status} == 0, "no sector hash mismatch detected");
}

######################################################################
# Get the tracer and corruptor.
#
# @return The tracer and corruptor
##
sub getDevices {
  my ($self) = assertNumArgs(1, @_);
  my $tracer = $self->getDevice();
  assertDefined($tracer, "device exists");
  assertType('Permabit::BlockDevice::TestDevice::Managed::Tracer', $tracer);

  my $corruptor = $tracer->getStorageDevice();
  assertDefined($corruptor, "tracer is built upon a source device");
  assertType('Permabit::BlockDevice::TestDevice::Managed::Corruptor',
             $corruptor);
  return ($tracer, $corruptor);
}

1;